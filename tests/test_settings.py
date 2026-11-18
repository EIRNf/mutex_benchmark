import dataclasses
import unittest

from scripts.config import RunPaths, load_config
from scripts.rundir import expected_data_files
from scripts.runner import get_command, get_data_file_name
from scripts.settings import resolve_run


def run_for(data_folder, *overrides):
    cfg = load_config(None, ["capture=throughput", "sweep.var=threads", "sweep.start=1",
                             "sweep.stop=2", "sweep.step=1", *overrides])
    paths = RunPaths(data=data_folder, plots="p", logs="l", run_dir=None)
    return resolve_run(cfg, paths, skip_build=False)


class RunSettingsTests(unittest.TestCase):
    def test_runs_in_one_process_do_not_share_state(self):
        native = run_for("a", "locks.include=spin", "memory.placement=process", "memory.node=0")
        tigon = run_for("b", "bench=tigon_ycsb", "locks.include=mcs", "memory.placement=locks",
                        "memory.node=1", "tigon.partition_ratio=half")

        # Interleaved use: each call reflects only the settings it was given.
        self.assertEqual(get_command(native, "spin", threads=2)[:3],
                         ["numactl", "--membind=0", "build/apps/max_contention_bench/max_contention_bench"])
        tigon_command = get_command(tigon, "mcs", threads=2)
        self.assertEqual(tigon_command[:3], ["./scripts/with_lock.sh", "-b", "build-hardware_cxl"])
        self.assertIn("--partition_num=1", tigon_command)
        self.assertEqual(get_data_file_name(native, "spin", 0, threads=2),
                         "a/spin-max-throughput-membind-node0-rep0-threads=2.csv")
        self.assertTrue(get_data_file_name(tigon, "mcs", 0, threads=2).startswith(
            "b/mcs-tigon_ycsb-query-rmw-protocol-TwoPL-partition-ratio-half-"))
        self.assertEqual(len(expected_data_files(native)), 20)  # 1 lock x 2 thread counts x 10 reps

    def test_settings_are_immutable(self):
        run = run_for("a")
        with self.assertRaises(dataclasses.FrozenInstanceError):
            run.bench = "min"
        self.assertEqual(dataclasses.replace(run, bench_n_seconds=5.0).bench_n_seconds, 5.0)
        self.assertEqual(run.bench_n_seconds, 1.0)

    def test_no_sweep_leaves_sweep_fields_empty(self):
        run = resolve_run(load_config(None, []), skip_build=False)
        self.assertEqual((run.iter, run.iter_variable_name, run.iter_range), (False, None, None))


if __name__ == "__main__":
    unittest.main()
