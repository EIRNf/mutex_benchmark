"""The max benchmark's workload settings: added work, warmup, pinning, fairness."""

import os
import tempfile
import tomllib
import unittest
from pathlib import Path
from unittest.mock import patch

import numpy as np
import pandas as pd

from scripts.config import (
    ConfigError,
    RunPaths,
    check_new_run,
    load_config,
    run_title,
    to_toml,
    with_run_defaults,
)
from scripts.dataloader import read_metadata, read_thread_level_csv
from scripts.fairness import execution_metrics, thread_level_metrics
from scripts.plotter import describe_workload
from scripts.runner import _bench_timeout, get_command, get_data_file_name
from scripts.settings import resolve_run
from scripts.suite import suite_from_data
from scripts.topology import cpu_order, parse_cpu_list, pinned_cpus, read_topology

THREADS_SWEEP = ["bench=max", "capture=throughput", "sweep.var=threads", "sweep.start=1",
                 "sweep.stop=2", "sweep.step=1", "locks.include=ticket"]


def config(*overrides):
    return load_config(None, [*THREADS_SWEEP, *overrides])


def new_run(*overrides, data="d"):
    return resolve_run(config(*overrides), RunPaths(data=data, plots="p", logs="l"), skip_build=False)


class WorkloadConfigTests(unittest.TestCase):
    def test_new_runs_record_warmup_and_pinning_but_old_configs_stay_unset(self):
        cfg = config()
        self.assertIsNone(cfg.workload.warmup)  # what an old config.toml loads as
        filled = with_run_defaults(cfg)
        self.assertEqual((filled.workload.warmup, filled.workload.pin), (0.5, "none"))
        self.assertIn("warmup = 0.5", to_toml(filled))
        self.assertEqual(new_run().warmup_seconds, 0.5)
        replot = resolve_run(cfg, RunPaths(data="d", plots="p", logs="l", reuse=True))
        self.assertIsNone(replot.warmup_seconds)
        self.assertIn("no warmup recorded", describe_workload(replot))
        # Other benches have no warmup setting.
        self.assertIsNone(with_run_defaults(load_config(None, ["bench=min"])).workload.warmup)

    def test_workload_keys_are_checked(self):
        for overrides, message in [
            (["bench=min", "workload.critical_ns=5"], "only applies to bench max"),
            (["workload.critical_ns=-1"], "must be >= 0"),
            (["workload.warmup=-0.5"], "`workload.warmup` must be >= 0"),
            (["workload.pin=spread"], "`workload.pin` must be one of none, compact, scatter"),
            (["capture=latency", "sweep.var=critical_ns", "sweep.start=0", "sweep.stop=1",
              "plot.layout=none", "workload.critical_delay=5"], "not both"),
        ]:
            with self.subTest(overrides=overrides), self.assertRaisesRegex(ConfigError, message):
                config(*overrides)

    def test_legacy_delays_are_refused_for_new_throughput_runs_but_old_ones_load(self):
        old = config("workload.critical_delay=100")  # a recorded run can still be re-plotted
        with self.assertRaisesRegex(ConfigError, "only affects `capture = latency`; use `workload.critical_ns`"):
            check_new_run(old)
        replot = resolve_run(old, RunPaths(data="d", plots="p", logs="l", reuse=True))
        self.assertIn("had no effect on throughput runs", describe_workload(replot))
        check_new_run(load_config(None, ["capture=latency", "workload.critical_delay=100"]))

    def test_the_run_title_declares_added_work_and_pinning(self):
        self.assertEqual(run_title(config("name=max")), "max")
        self.assertEqual(run_title(config("name=max", "workload.critical_ns=200",
                                          "workload.noncritical_ns=1000", "workload.pin=compact")),
                         "max-cs200ns-ncs1000ns-pin-compact")
        swept = load_config(None, ["name=cs", "capture=throughput", "sweep.var=critical_ns",
                                   "sweep.start=0", "sweep.stop=2000", "sweep.step=500"])
        self.assertEqual(run_title(swept), "cs-cs0-2000ns")


class CommandTests(unittest.TestCase):
    def test_the_roof_command_adds_only_the_warmup(self):
        command = get_command(new_run(), "ticket", threads=2, thread_level=True)
        self.assertEqual(command[1:], ["ticket", "2", "1.0", "--warmup", "0.5", "--csv", "--thread-level"])
        self.assertEqual(get_data_file_name(new_run(), "ticket", 0, threads=2),
                         "d/ticket-max-throughput-rep0-threads=2.csv")
        self.assertIn("Roof: no added work · unpinned · warmup 0.5 s", describe_workload(new_run()))

    def test_added_work_and_pinning_reach_the_command_and_file_names(self):
        run = new_run("workload.critical_ns=200", "workload.noncritical_ns=1000",
                      "workload.pin=compact", "workload.warmup=0")
        with patch("scripts.topology.host_cpu_order", return_value=[0, 2, 1, 3]):
            command = get_command(run, "ticket", threads=6, thread_level=True)
        self.assertIn("--critical-ns 200 --noncritical-ns 1000 --cpus 0,2,1,3,0,2", " ".join(command))
        self.assertNotIn("--warmup", command)
        self.assertEqual(get_data_file_name(run, "ticket", 0, threads=6),
                         "d/ticket-max-throughput-cs200ns-ncs1000ns-pin-compact-rep0-threads=6.csv")
        self.assertIn("critical section 200 ns, non-critical ≈1 µs (uniform 0-2 µs) · pinned compact",
                      describe_workload(run))

    def test_a_swept_work_variable_is_passed_per_point(self):
        run = resolve_run(load_config(None, [
            "capture=throughput", "threads=4", "sweep.var=noncritical_ns",
            "sweep.start=0", "sweep.stop=1000", "sweep.step=500"]), RunPaths("d", "p", "l"))
        self.assertNotIn("--noncritical-ns", get_command(run, "ticket", noncritical_ns=0))
        self.assertIn("1000", get_command(run, "ticket", noncritical_ns=1000))
        self.assertEqual(get_data_file_name(run, "ticket", 0, noncritical_ns=500),
                         "d/ticket-max-throughput-rep0-noncritical_ns=500.csv")

    def test_the_timeout_allows_for_the_warmup(self):
        self.assertEqual(_bench_timeout(new_run("seconds=10", "workload.warmup=2")), 135)


class TopologyTests(unittest.TestCase):
    def fake_sysfs(self, root):
        """Two sockets x two cores x two hyperthreads; CPUs numbered as on many Intel servers."""
        layout = {  # cpu: (package, siblings)
            0: (0, "0,4"), 1: (0, "1,5"), 2: (1, "2,6"), 3: (1, "3,7"),
            4: (0, "0,4"), 5: (0, "1,5"), 6: (1, "2,6"), 7: (1, "3,7"),
        }
        Path(root, "cpu").mkdir()
        Path(root, "cpu", "online").write_text("0-7\n")
        for cpu, (package, siblings) in layout.items():
            topo = Path(root, "cpu", f"cpu{cpu}", "topology")
            topo.mkdir(parents=True)
            topo.joinpath("physical_package_id").write_text(f"{package}\n")
            topo.joinpath("thread_siblings_list").write_text(siblings + "\n")
        for node, cpus in ((0, "0-1,4-5"), (1, "2-3,6-7")):
            Path(root, "node", f"node{node}").mkdir(parents=True)
            Path(root, "node", f"node{node}", "cpulist").write_text(cpus + "\n")

    def test_compact_fills_a_socket_and_scatter_alternates_with_smt_last(self):
        with tempfile.TemporaryDirectory() as root:
            self.fake_sysfs(root)
            cpus = read_topology(root)
        self.assertEqual(cpu_order("compact", cpus), [0, 1, 2, 3, 4, 5, 6, 7])
        self.assertEqual(cpu_order("scatter", cpus), [0, 2, 1, 3, 4, 6, 5, 7])
        self.assertEqual([c.as_list() for c in cpus[4:5]], [[4, 0, 0, 1, 0]])

    def test_only_cpus_the_process_may_use_are_ordered(self):
        with tempfile.TemporaryDirectory() as root:
            self.fake_sysfs(root)
            cpus = read_topology(root, allowed={1, 2, 3, 5})
        self.assertEqual(cpu_order("compact", cpus), [1, 2, 3, 5])
        self.assertEqual(pinned_cpus([1, 2, 3, 5], 6), [1, 2, 3, 5, 1, 2])
        self.assertEqual(parse_cpu_list("0-2,8,10-11\n"), [0, 1, 2, 8, 10, 11])


class DataTests(unittest.TestCase):
    def test_new_and_legacy_thread_level_files_load_alike(self):
        with tempfile.TemporaryDirectory() as tmp:
            new = Path(tmp, "new.csv")
            new.write_text(
                "# max_contention_bench mode=thread-level warmup_s=0.5 reacquires=3\n"
                "thread_id,seconds,iterations,reacquires,cpu_pinned,cpu_start,cpu_end,node_end\n"
                "0,1.0,100,1,4,4,4,0\n1,1.0,300,2,5,5,5,0\n")
            old = Path(tmp, "old.csv")
            old.write_text("0,1.0,100\n1,1.0,300\n")
            new_df, old_df = read_thread_level_csv(str(new)), read_thread_level_csv(str(old))
            self.assertEqual(list(new_df["# Iterations"]), list(old_df["# Iterations"]))
            self.assertEqual(list(new_df["Reacquires"]), [1, 2])
            self.assertEqual(list(new_df["CPU pinned"]), [4, 5])
            self.assertEqual(read_metadata(str(new))["warmup_s"], "0.5")
            self.assertEqual(read_metadata(str(old)), {})


class FairnessTests(unittest.TestCase):
    def test_metrics_of_one_execution(self):
        fair = execution_metrics([100, 100, 100, 100], reacquires=0)
        self.assertEqual((fair["jain"], fair["slowest_share"], fair["reacquire_share"]), (1.0, 1.0, 0.0))
        hog = execution_metrics([400, 0, 0, 0], reacquires=399)
        self.assertAlmostEqual(hog["jain"], 0.25)       # 1/n: one thread did everything
        self.assertEqual(hog["slowest_share"], 0.0)
        self.assertAlmostEqual(hog["reacquire_share"], 399 / 400)
        self.assertTrue(np.isnan(execution_metrics([50])["jain"]))  # one thread: undefined
        self.assertTrue(np.isnan(execution_metrics([1, 3])["reacquire_share"]))  # not recorded

    def test_per_execution_rows_from_loaded_sweep_data(self):
        df = pd.DataFrame({
            "Thread ID": [0, 0, 1, 0, 1], "# Iterations": [9, 5, 5, 6, 2],
            "Reacquires": [8, 0, 0, 1, 1], "run": [0, 0, 0, 1, 1], "threads": [1, 2, 2, 2, 2],
        })
        rows = thread_level_metrics([("ticket", df)], "threads").set_index(["x", "rep"])
        self.assertTrue(np.isnan(rows.loc[(1, 0), "jain"]))
        self.assertEqual(rows.loc[(2, 0), "jain"], 1.0)
        self.assertAlmostEqual(rows.loc[(2, 1), "slowest_share"], 2 / 4)
        self.assertAlmostEqual(rows.loc[(2, 1), "reacquire_share"], 2 / 8)


class SuiteNamingTests(unittest.TestCase):
    SUITE = """
[defaults]
bench = "max"
capture = "throughput"
locks = { include = ["ticket"] }
sweep = { var = "threads", start = 1, stop = 2, step = 1 }

[[experiments]]
vary = { "workload.critical_ns" = [0, 200], "workload.pin" = ["none", "scatter"] }
"""

    def test_run_keys_declare_added_work_and_pinning(self):
        keys = [run.key for run in suite_from_data(tomllib.loads(self.SUITE)).runs]
        self.assertEqual(keys, ["max", "max-pin-scatter", "max-cs200ns", "max-cs200ns-pin-scatter"])

    def test_work_set_by_an_override_is_declared_too(self):
        runs = suite_from_data(tomllib.loads(self.SUITE.replace(', "workload.pin" = ["none", "scatter"]', "")),
                               ["workload.noncritical_ns=1000"]).runs
        self.assertEqual([run.key for run in runs], ["max-ncs1000ns", "max-cs200ns-ncs1000ns"])


if __name__ == "__main__":
    unittest.main()
