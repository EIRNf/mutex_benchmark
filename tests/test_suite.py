import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from scripts.config import ConfigError, load_config
from scripts.rundir import SUITE_CONTEXT_ENV
from scripts.suite import (
    SuiteOptions,
    SuiteRunner,
    describe_suite,
    executions,
    load_suite,
    plan_builds,
    suite_from_data,
    suite_from_record,
)
import tomllib

ROOT = Path(__file__).resolve().parents[1]
PCI = "0000:a0:00.0"

SUITE = """
name = "demo"

[defaults]
capture = "throughput"
seconds = 1
repetitions = 1
locks = { include = ["spin", "ticket"] }
sweep = { var = "threads", start = 1, stop = 2, step = 1 }
plot = { layout = "none" }

[[environments]]
name = "near"
memory = { placement = "process", node = 0 }

[[environments]]
name = "far"
memory = { placement = "locks", node = 1 }

[[experiments]]
bench = "max"

[[experiments]]
bench = "min"
"""

LATENCY_SUITE = f"""
[defaults]
capture = "throughput"
seconds = 1
repetitions = 1
locks = {{ include = ["spin"] }}
sweep = {{ var = "threads", start = 1, stop = 1, step = 1 }}
plot = {{ layout = "none" }}

[latency_control]
pci = "{PCI}"

[[environments]]
name = "host"

[[environments]]
name = "slow"
latency = "ffff"

[[environments]]
name = "fast"
latency = "0x0020"

[[experiments]]
bench = "max"
"""


def suite(text=SUITE, overrides=(), base_dir="."):
    return suite_from_data(tomllib.loads(text), overrides, base_dir=base_dir)


class ExpansionTests(unittest.TestCase):
    def test_every_example_suite_is_valid(self):
        examples = sorted((ROOT / "experiments" / "suites").glob("*.toml"))
        self.assertTrue(examples)
        for path in examples:
            with self.subTest(path=path.name):
                self.assertTrue(load_suite(str(path)).runs)

    def test_environments_times_experiments(self):
        s = suite()
        self.assertEqual([run.key for run in s.runs], ["near/max", "near/min", "far/max", "far/min"])
        far_min = s.runs[3]
        self.assertEqual(far_min.cfg.name, "far-min")
        self.assertEqual((far_min.cfg.memory.placement, far_min.cfg.memory.node), ("locks", 1))
        self.assertEqual(far_min.build_dir, "build-hardware_cxl")
        self.assertEqual(s.runs[0].build_dir, "build")
        self.assertEqual(far_min.cfg.locks.include, ["spin", "ticket"])  # from defaults

    def test_overrides_apply_to_every_run(self):
        s = suite(overrides=["seconds=5", "locks.include=mcs"])
        self.assertTrue(all(run.cfg.seconds == 5.0 for run in s.runs))
        self.assertTrue(all(run.cfg.locks.include == ["mcs"] for run in s.runs))

    def test_vary_expands_one_entry_into_named_experiments(self):
        s = suite("""
            [defaults]
            capture = "throughput"
            sweep = { var = "threads", start = 1, stop = 4, step = 1 }
            plot = { layout = "none" }
            [[experiments]]
            bench = "tigon_ycsb"
            vary = { "tigon.partition_ratio" = ["threads", "half"], tigon = { zipf = [0.0, 0.9] } }
        """)
        self.assertEqual([run.key for run in s.runs], [
            "tigon_ycsb-partition_ratio-threads-zipf-0",
            "tigon_ycsb-partition_ratio-threads-zipf-0.9",
            "tigon_ycsb-partition_ratio-half-zipf-0",
            "tigon_ycsb-partition_ratio-half-zipf-0.9",
        ])
        self.assertEqual((s.runs[3].cfg.tigon.partition_ratio, s.runs[3].cfg.tigon.zipf), ("half", 0.9))

    def test_experiment_files_are_layered_between_defaults_and_entry(self):
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "base.toml").write_text(
                'bench = "min"\ncapture = "throughput"\nseconds = 3\n'
                '[sweep]\nvar = "threads"\nstart = 1\nstop = 2\nstep = 1\n',
                encoding="utf-8",
            )
            s = suite("""
                [defaults]
                seconds = 9
                repetitions = 2
                plot = { layout = "none" }
                [[experiments]]
                file = "base.toml"
                [[experiments]]
                file = "base.toml"
                name = "longer"
                seconds = 30
            """, base_dir=tmp)
        base, longer = s.runs
        self.assertEqual((base.key, base.cfg.bench, base.cfg.seconds, base.cfg.repetitions), ("base", "min", 3.0, 2))
        self.assertEqual((longer.key, longer.cfg.seconds), ("longer", 30.0))

    def test_mistakes_are_reported_with_their_location(self):
        cases = [
            ('bench = "max"\n[[experiments]]\n', "`bench` is an experiment setting"),
            ("[[experiments]]\n[[experiments]]\n", "two experiments are named 'max'; give one a `name`"),
            ('[[environments]]\nname = "a"\nmemroy = {}\n[[experiments]]\n',
             "unknown suite setting `environments\\[0\\].memroy`; did you mean `environments\\[0\\].memory`"),
            ('[[environments]]\nname = "a"\nlatency = "ff"\n[[experiments]]\n',
             "needs a \\[latency_control\\] table"),
            (f'[latency_control]\npci = "{PCI}"\n[[environments]]\nname = "a"\nlatency = 255\n[[experiments]]\n',
             "must be a quoted hex register value"),
            ('[latency_control]\npci = "a0:00"\n[[experiments]]\n', "must be the CXL device's PCI address"),
            ('[[environments]]\nname = "a"\nmemory = { placement = "process", node = 0 }\n'
             '[[experiments]]\nmemory = { node = 1 }\n', "experiment 'max' sets `memory`, which environment 'a'"),
            ('[[experiments]]\nvary = { "sweep.stopp" = [1, 2] }\n', "did you mean `sweep.stop`"),
            ('[[experiments]]\nbench = "grouped"\n', "run grouped: `workload.groups` must be set"),
            ("name = \"a b\"\n[[experiments]]\n", "`name` must start with a letter or digit"),
        ]
        for text, pattern in cases:
            with self.subTest(pattern=pattern):
                with self.assertRaisesRegex(ConfigError, pattern):
                    suite(text)


class PlanTests(unittest.TestCase):
    def test_one_build_per_folder_covers_every_lock_and_app(self):
        s = suite(SUITE.replace('include = ["spin", "ticket"]', 'include = ["spin", "futex"]'))
        plans = {plan.dir: plan for plan in plan_builds(s.runs)}
        self.assertEqual(set(plans), {"build", "build-hardware_cxl"})
        self.assertTrue(plans["build-hardware_cxl"].hardware_cxl)
        self.assertIn("futex", plans["build"].mutex_names)
        self.assertFalse(plans["build"].tigon)

    def test_listing_counts_executions_and_time(self):
        s = suite()
        self.assertEqual(executions(s.runs[0].cfg), 4)  # 2 locks x 2 thread counts x 1 rep
        text = describe_suite(s)
        self.assertIn("4 runs (2 environments x 2 experiments)", text)
        self.assertIn("Total: 16 benchmark executions, about 0m 16s", text)
        self.assertIn("build-hardware_cxl/   -Dhardware_cxl -lnuma -std=c++20", text)


class FakeChild:
    """Stands in for `python -m scripts run`: writes run.json with a chosen status."""

    def __init__(self, outcomes=None, interrupt_on=None):
        self.outcomes = outcomes or {}
        self.interrupt_on = interrupt_on
        self.calls = []

    def __call__(self, command, env):
        folder = command[command.index("--dir") + 1]
        context = json.loads(env[SUITE_CONTEXT_ENV])
        self.calls.append((context["run"], command, context))
        if context["run"] == self.interrupt_on:
            self._write(folder, "interrupted")
            raise KeyboardInterrupt
        status = self.outcomes.get(context["run"], "complete")
        if status is None:  # crashed before recording anything
            return 1
        self._write(folder, status)
        return 0 if status == "complete" else 1

    @staticmethod
    def _write(folder, status):
        record = {"status": status, "data_points": {"expected": 2, "written": 2 if status == "complete" else 1}}
        if status == "failed":
            record["error"] = "RuntimeError: benchmark exploded"
        Path(folder, "run.json").write_text(json.dumps(record), encoding="utf-8")


class FakeRegister:
    """A CXL latency register behind a fake setpci."""

    def __init__(self, value="00000040"):
        self.value = value
        self.writes = []

    def __call__(self, command):
        assert command[:3] == ["setpci", "-s", PCI], command
        register, _, value = command[3].partition("=")
        assert register == "0xe08.l"
        if value:
            self.writes.append(value)
            self.value = value.zfill(8)
            return ""
        return self.value + "\n"


@patch("scripts.suite.check_host", lambda cfg: None)
@patch("scripts.suite.configured_cpp_args", lambda build_dir: ["-std=c++20"])
class RunnerTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.suite_dir = os.path.join(self.tmp.name, "suite")

    def runner(self, s, child, options=None, setpci=None, create=True):
        runner = SuiteRunner(s, self.suite_dir, options, child=child, setpci=setpci, out=io.StringIO())
        if create:
            runner.create(source="demo.toml")
        return runner

    def record(self):
        return json.loads(Path(self.suite_dir, "suite.json").read_text(encoding="utf-8"))

    def test_runs_every_experiment_in_its_own_prepared_folder(self):
        child = FakeChild()
        with patch("scripts.suite.build") as build:
            code = self.runner(suite(), child).run()
        self.assertEqual(code, 0)
        self.assertEqual([call[0] for call in child.calls], ["near/max", "near/min", "far/max", "far/min"])

        # Each build folder is built once, before any run, with every lock its runs need.
        self.assertEqual([c.args[0] for c in build.call_args_list], ["build", "build-hardware_cxl"])
        self.assertEqual(build.call_args_list[1].kwargs["mutex_names"], ["spin", "ticket"])

        key, command, context = child.calls[2]
        folder = os.path.join(self.suite_dir, "far", "max")
        self.assertEqual(command[3:], ["run", os.path.join(folder, "config.toml"), "--dir", folder,
                                       "--build-dir", "build-hardware_cxl", "--no-build"])
        self.assertEqual(load_config(os.path.join(folder, "config.toml")).name, "far-max")
        self.assertEqual(context["environment"]["memory"], {"placement": "locks", "node": 1})

        record = self.record()
        self.assertEqual(record["status"], "complete")
        self.assertEqual({run["status"] for run in record["runs"]}, {"complete"})
        self.assertEqual(record["sessions"][0]["status"], "complete")
        self.assertEqual(Path(self.suite_dir, "suite.toml").read_text(encoding="utf-8"), "")

    def test_failures_do_not_stop_the_suite_and_resume_reruns_only_them(self):
        child = FakeChild({"near/min": "failed", "far/max": None})
        with patch("scripts.suite.build"):
            self.assertEqual(self.runner(suite(), child).run(), 1)
        self.assertEqual(len(child.calls), 4)
        runs = {run["key"]: run for run in self.record()["runs"]}
        self.assertEqual(runs["near/min"]["error"], "RuntimeError: benchmark exploded")
        self.assertEqual(runs["far/max"]["status"], "failed")
        self.assertIn("exited before recording", runs["far/max"]["error"])
        self.assertEqual(self.record()["status"], "incomplete")

        retry = FakeChild()
        with patch("scripts.suite.build"):
            resumed = SuiteRunner(suite_from_record(self.suite_dir), self.suite_dir,
                                  child=retry, out=io.StringIO())
            self.assertEqual(resumed.run(), 0)
        self.assertEqual([call[0] for call in retry.calls], ["near/min", "far/max"])
        self.assertTrue(Path(self.suite_dir, "near", "min.attempt-1", "run.json").exists())
        record = self.record()
        self.assertEqual(record["status"], "complete")
        self.assertEqual(len(record["sessions"]), 2)
        runs = {run["key"]: run for run in record["runs"]}
        self.assertEqual(runs["near/min"]["attempts"], [os.path.join("near", "min.attempt-1")])
        self.assertNotIn("error", runs["near/min"])

    def test_stop_on_failure_and_only(self):
        child = FakeChild({"near/max": "failed"})
        with patch("scripts.suite.build"):
            runner = self.runner(suite(), child, SuiteOptions(only=["max"], stop_on_failure=True))
            self.assertEqual([run.key for run in runner.selected_runs()], ["near/max", "far/max"])
            self.assertEqual(runner.run(), 1)
        self.assertEqual([call[0] for call in child.calls], ["near/max"])
        with self.assertRaisesRegex(ConfigError, "no runs match --only nope"):
            SuiteRunner(suite(), self.suite_dir, SuiteOptions(only=["nope"])).selected_runs()

    def test_latency_is_set_per_environment_and_always_restored(self):
        register = FakeRegister("00000040")
        child = FakeChild()
        with patch("scripts.suite.build"):
            self.assertEqual(self.runner(suite(LATENCY_SUITE), child, setpci=register).run(), 0)
        self.assertEqual(register.writes, ["ffff", "0020", "00000040"])
        self.assertEqual(register.value, "00000040")
        contexts = {key: context["environment"]["cxl_latency"] for key, _, context in child.calls}
        self.assertEqual(contexts["host/max"], {"pci": PCI, "register": "0xe08.l",
                                                "value": "00000040", "host_setting": True})
        self.assertEqual(contexts["slow/max"]["value"], "ffff")
        self.assertEqual(self.record()["sessions"][0]["latency_original"], "00000040")

    def test_interrupt_restores_latency_and_marks_the_suite(self):
        register = FakeRegister("00000040")
        child = FakeChild(interrupt_on="slow/max")
        with patch("scripts.suite.build"), self.assertRaises(KeyboardInterrupt):
            self.runner(suite(LATENCY_SUITE), child, setpci=register).run()
        self.assertEqual(register.value, "00000040")
        record = self.record()
        self.assertEqual(record["status"], "interrupted")
        self.assertEqual({run["key"]: run["status"] for run in record["runs"]},
                         {"host/max": "complete", "slow/max": "interrupted", "fast/max": "pending"})

    def test_a_suite_folder_is_never_reused(self):
        with patch("scripts.suite.build"):
            self.runner(suite(), FakeChild())
            with self.assertRaisesRegex(ConfigError, "already holds a suite"):
                self.runner(suite(), FakeChild())


class SuiteCommandTests(unittest.TestCase):
    def run_cli(self, *argv):
        return subprocess.run(
            [sys.executable, "-m", "scripts", "suite", *argv],
            cwd=ROOT, capture_output=True, text=True, check=False,
        )

    def test_list_shows_the_plan_without_creating_anything(self):
        with tempfile.TemporaryDirectory() as tmp:
            target = os.path.join(tmp, "suite")
            result = self.run_cli("experiments/suites/placement.toml", "--list", "--dir", target,
                                  "--only", "cxl-locks", "-o", "seconds=1")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("cxl-locks/max", result.stdout)
            self.assertIn("--only selects 2 of 6 runs: cxl-locks/max, cxl-locks/min", result.stdout)
            self.assertFalse(os.path.exists(target))

    def test_usage_errors(self):
        result = self.run_cli()
        self.assertEqual(result.returncode, 2)
        self.assertIn("give a suite file, or --resume", result.stderr)
        result = self.run_cli("--resume", "somewhere", "-o", "seconds=1")
        self.assertEqual(result.returncode, 2)
        self.assertIn("-o and --dir cannot change it", result.stderr)
        result = self.run_cli("experiments/suites/placement.toml", "--list", "-o", "bench=nope")
        self.assertEqual(result.returncode, 2)
        self.assertIn("run dram/max: `bench` must be one of", result.stderr)


if __name__ == "__main__":
    unittest.main()
