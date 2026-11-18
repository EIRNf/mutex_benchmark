import io
import json
import os
import subprocess
import sys
import tempfile
import tomllib
import unittest
from pathlib import Path

import pandas as pd

from scripts.compare import compare_suite
from scripts.config import ConfigError, RunPaths, apply_to_constants
from scripts.runner import get_data_file_name
from scripts.suite import SuiteRunner, suite_from_data

ROOT = Path(__file__).resolve().parents[1]

SUITE = """
name = "cmp"

[defaults]
capture = "throughput"
seconds = 2
repetitions = 2
locks = { include = ["spin", "ticket"] }
sweep = { var = "threads", start = 1, stop = 3, step = 1 }
plot = { layout = "none" }

[[environments]]
name = "dram"
memory = { placement = "process", node = 0 }

[[environments]]
name = "cxl"
memory = { placement = "process", node = 1 }

[[experiments]]
bench = "max"

[[experiments]]
bench = "min"
"""

BASE = {"max": 1000, "min": 400}             # ops/s per thread on DRAM
CXL_FACTOR = {"max": 0.5, "min": 0.25}        # CXL throughput / DRAM throughput


def throughput(environment, experiment, threads, rep):
    factor = CXL_FACTOR[experiment] if environment == "cxl" else 1.0
    return BASE[experiment] * factor * threads * (1 + 0.1 * rep)


def make_suite(suite_dir, text=SUITE, skip=(), status=None):
    """A suite folder whose runs hold synthetic per-thread CSVs."""
    suite = suite_from_data(tomllib.loads(text))
    SuiteRunner(suite, suite_dir, out=io.StringIO()).create()
    for run in suite.runs:
        if run.key in skip:
            continue
        run_dir = os.path.join(suite_dir, *run.key.split("/"))
        os.makedirs(os.path.join(run_dir, "raw"))
        apply_to_constants(run.cfg, RunPaths.for_existing_run(run_dir))
        seconds = run.cfg.seconds
        for rep in range(run.cfg.repetitions):
            for threads in range(run.cfg.sweep.start, run.cfg.sweep.stop + 1, run.cfg.sweep.step):
                total = throughput(run.environment, run.experiment, threads, rep) * seconds
                for lock in ("spin", "ticket"):
                    with open(get_data_file_name(lock, rep, threads=threads), "w", encoding="utf-8") as handle:
                        for thread in range(threads):  # split the total evenly over the threads
                            handle.write(f"{thread},{seconds},{total / threads}\n")
        record = {"status": (status or {}).get(run.key, "complete")}
        Path(run_dir, "run.json").write_text(json.dumps(record), encoding="utf-8")
    return suite


class CompareTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.suite_dir = os.path.join(self.tmp.name, "suite")

    def stats(self, result, group, series):
        frame = result.stats[group]
        return frame[frame["series"] == series].set_index(["lock", "x"])

    def test_each_experiment_is_compared_across_environments(self):
        make_suite(self.suite_dir)
        result = compare_suite(self.suite_dir)
        self.assertEqual((result.by, result.baseline), ("environment", "dram"))
        self.assertEqual([g.name for g in result.groups], ["max", "min"])

        dram = self.stats(result, "max", "dram")
        self.assertAlmostEqual(dram.loc[("spin", 2), "mean"], 2100.0)   # (2000 + 2200) / 2
        self.assertEqual(dram.loc[("spin", 2), "n"], 2)
        self.assertTrue((dram["ratio"] == 1.0).all())
        self.assertTrue(self.stats(result, "max", "cxl")["ratio"].round(9).eq(0.5).all())
        self.assertTrue(self.stats(result, "min", "cxl")["ratio"].round(9).eq(0.25).all())

        summary = result.summary.set_index(["group", "lock"])["geomean_ratio"]
        self.assertAlmostEqual(summary[("max", "ticket")], 0.5)
        self.assertAlmostEqual(summary[("min", "spin")], 0.25)

        out = Path(self.suite_dir, "compare")
        self.assertEqual(sorted(p.name for p in out.iterdir()), [
            "max-throughput.png", "max-vs-dram.png", "max.csv",
            "min-throughput.png", "min-vs-dram.png", "min.csv",
            "summary-vs-dram.png", "summary.csv",
        ])
        csv = pd.read_csv(out / "max.csv")
        self.assertEqual(list(csv.columns), ["series", "lock", "threads", "mean", "std", "n", "ratio"])
        self.assertEqual(len(csv), 2 * 2 * 3)  # environments x locks x thread counts

    def test_baseline_and_direction_can_change(self):
        make_suite(self.suite_dir)
        result = compare_suite(self.suite_dir, baseline="cxl")
        self.assertTrue(self.stats(result, "max", "dram")["ratio"].round(9).eq(2.0).all())

        by_experiment = compare_suite(self.suite_dir, by="experiment", out_dir=os.path.join(self.tmp.name, "x"))
        self.assertEqual([g.name for g in by_experiment.groups], ["dram", "cxl"])
        self.assertEqual(by_experiment.baseline, "max")
        self.assertTrue(self.stats(by_experiment, "dram", "min")["ratio"].round(9).eq(0.4).all())
        self.assertTrue(self.stats(by_experiment, "cxl", "min")["ratio"].round(9).eq(0.2).all())

    def test_unfinished_and_unpaired_runs_are_reported(self):
        make_suite(self.suite_dir, skip={"cxl/min"}, status={"cxl/max": "interrupted"})
        result = compare_suite(self.suite_dir)
        self.assertEqual([g.name for g in result.groups], ["max"])
        self.assertIn("cxl/min: skipped, not run yet", result.notes)
        self.assertIn("min: skipped, needs at least two runs to compare", result.notes)
        self.assertIn("cxl/max: included with the data it wrote (status interrupted)", result.notes)

        only = compare_suite(self.suite_dir, only=["max"], out_dir=os.path.join(self.tmp.name, "y"))
        self.assertEqual([g.name for g in only.groups], ["max"])
        self.assertNotIn("cxl/min: skipped, not run yet", only.notes)

    def test_mistakes(self):
        make_suite(self.suite_dir)
        with self.assertRaisesRegex(ConfigError, "--baseline 'nvm' is not one of the compared environments"):
            compare_suite(self.suite_dir, baseline="nvm")
        with self.assertRaisesRegex(ConfigError, "nothing to compare"):
            compare_suite(self.suite_dir, only=["nope"])
        single = os.path.join(self.tmp.name, "single")
        text = SUITE.split("[[environments]]")[0] + '[[experiments]]\nbench = "max"\n'
        make_suite(single, text)
        with self.assertRaisesRegex(ConfigError, "fewer than two environments"):
            compare_suite(single, by="environment")

    def test_command_line(self):
        make_suite(self.suite_dir)
        env = {**os.environ, "MPLBACKEND": "Agg"}
        result = subprocess.run([sys.executable, "-m", "scripts", "compare", self.suite_dir],
                                cwd=ROOT, env=env, capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Compared by environment against dram: 2 group(s): max, min", result.stdout)
        result = subprocess.run([sys.executable, "-m", "scripts", "compare", self.suite_dir, "--baseline", "nvm"],
                                cwd=ROOT, env=env, capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 2)
        self.assertIn("is not one of the compared environments", result.stderr)


if __name__ == "__main__":
    unittest.main()
