import contextlib
import io
import subprocess
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

from scripts.args import build_parser, config_from_args, init_args, render_for_cli
from scripts.config import (
    ConfigError,
    config_from_data,
    config_from_toml,
    load_config,
    to_toml,
)
from scripts.runner import get_command, get_data_file_name
from scripts.settings import resolve_run

ROOT = Path(__file__).resolve().parents[1]

THROUGHPUT = """
bench = "max"
capture = "throughput"
seconds = 2
repetitions = 3
[locks]
include = ["spin", "mcs"]
[sweep]
var = "threads"
start = 1
stop = 8
step = 1
[plot]
layout = "none"
"""


def flags_to_config(*argv):
    return config_from_args(build_parser().parse_args(list(argv)))


class ExampleConfigTests(unittest.TestCase):
    def test_every_example_config_is_valid(self):
        examples = sorted((ROOT / "experiments").glob("*.toml"))
        self.assertTrue(examples)
        for path in examples:
            with self.subTest(path=path.name):
                cfg = load_config(str(path))
                self.assertEqual(cfg.name, path.stem)


class OverrideTests(unittest.TestCase):
    def test_overrides_are_typed_by_the_schema(self):
        cfg = config_from_toml(THROUGHPUT, [
            "seconds=10",                      # int -> float field
            "sweep.stop=32",
            "locks.include=spin,ticket,mcs",   # comma list
            "plot.layout=faceted",             # bare word -> string
            "plot.variability=true",
        ])
        self.assertEqual(cfg.seconds, 10.0)
        self.assertEqual(cfg.sweep.stop, 32)
        self.assertEqual(cfg.locks.include, ["spin", "ticket", "mcs"])
        self.assertEqual(cfg.plot.layout, "faceted")
        self.assertTrue(cfg.plot.variability)

    def test_overrides_can_create_tables_missing_from_the_file(self):
        cfg = config_from_data({}, [
            "capture=throughput", "plot.layout=none",
            "sweep.var=threads", "sweep.start=1", "sweep.stop=4", "sweep.step=1",
        ])
        self.assertEqual((cfg.sweep.start, cfg.sweep.stop), (1, 4))

    def test_unknown_keys_suggest_the_closest_setting(self):
        with self.assertRaisesRegex(ConfigError, "did you mean `sweep.stop`"):
            config_from_toml(THROUGHPUT, ["sweep.stopp=4"])
        with self.assertRaisesRegex(ConfigError, "unknown setting `tigon.keyz`"):
            config_from_toml('bench = "tigon_ycsb"\n[tigon]\nkeyz = 5\n')

    def test_wrong_types_are_rejected(self):
        with self.assertRaisesRegex(ConfigError, "`seconds` must be a number"):
            config_from_toml(THROUGHPUT, ["seconds=soon"])
        with self.assertRaisesRegex(ConfigError, "`sweep` is missing `sweep.step`"):
            config_from_toml('capture = "throughput"\n[sweep]\nvar = "threads"\nstart = 1\nstop = 2\n')

    def test_resolved_config_round_trips_through_toml(self):
        cfg = config_from_toml(THROUGHPUT, ["memory.placement=process", "memory.node=1"])
        self.assertEqual(config_from_toml(to_toml(cfg)), cfg)


class ValidationTests(unittest.TestCase):
    def assertInvalid(self, text, overrides, pattern):
        with self.assertRaisesRegex(ConfigError, pattern):
            config_from_toml(text, overrides)

    def test_cross_field_rules(self):
        self.assertInvalid(THROUGHPUT, ["tigon.keys=5"], "only applies to bench tigon")
        self.assertInvalid(THROUGHPUT, ["memory.node=1"], "`memory.node` requires `memory.placement`")
        self.assertInvalid(THROUGHPUT, ["memory.placement=locks"], "requires `memory.node`")
        self.assertInvalid(THROUGHPUT, ["memory.placement=both", "memory.node=1"],
                           "`memory.placement` must be one of none, process, locks")
        self.assertInvalid(THROUGHPUT, ["bench=grouped"], "`workload.groups` must be set")
        self.assertInvalid(THROUGHPUT, ["bench=min", "workload.critical_delay=5"],
                           "only applies to bench max")
        self.assertInvalid('capture = "throughput"\n', [], "requires a `sweep`")
        self.assertInvalid(THROUGHPUT, ["locks.sets=nonexistent"], "unknown lock set 'nonexistent'")
        self.assertInvalid(
            THROUGHPUT, ["bench=tigon_tpcc", "tigon.query=rmw"],
            "`tigon.query` 'rmw' is not valid for bench tigon_tpcc",
        )


class FlagCliTests(unittest.TestCase):
    def test_flags_and_toml_build_the_same_config(self):
        from_flags = flags_to_config(
            "4", "2", "3", "--capture", "throughput", "--sweep", "threads", "1", "8", "1",
            "-i", "spin", "mcs", "--plot", "none", "--data-folder", "./tmp-config-test-data",
        )
        from_toml = config_from_toml(THROUGHPUT, ["threads=4"], name="max")
        self.assertEqual(from_flags, from_toml)

    def test_alloc_is_an_alias_for_placement(self):
        cfg = flags_to_config("4", "1", "1", "--alloc", "cxl-hardware", "--numa-node", "2")
        self.assertEqual((cfg.memory.placement, cfg.memory.node), ("locks", 2))
        with self.assertRaisesRegex(ConfigError, "conflicts"):
            flags_to_config("4", "1", "1", "--alloc", "cxl-hardware", "--placement", "process",
                            "--numa-node", "2")

    def test_errors_name_flags_not_config_keys(self):
        self.assertEqual(
            render_for_cli("`memory.node` requires `memory.placement` to be process or locks"),
            "--numa-node requires --placement to be process or locks",
        )
        self.assertEqual(
            render_for_cli("`plot.heatmap_ref` cannot be combined with `plot.heatmap = false`"),
            "--heatmap-ref cannot be combined with --no-heatmap",
        )
        self.assertEqual(
            render_for_cli("set either `workload.critical_delay` or `sweep.var = critical_delay`"),
            "set either --critical-delay or --sweep critical-delay",
        )
        stderr = io.StringIO()
        argv = ["scripts.main", "4", "1", "1", "--bench", "tigon_tpcc", "--tigon-query", "rmw",
                "--capture", "throughput", "--sweep", "threads", "1", "2", "1"]
        with patch.object(sys, "argv", argv), contextlib.redirect_stderr(stderr):
            with self.assertRaises(SystemExit):
                init_args()
        self.assertIn("--tigon-query 'rmw' is not valid", stderr.getvalue())


class PlacementTests(unittest.TestCase):
    def run_for(self, *overrides):
        return resolve_run(config_from_toml(THROUGHPUT, list(overrides)), skip_build=False)

    def test_process_placement_wraps_each_benchmark_in_numactl(self):
        run = self.run_for("memory.placement=process", "memory.node=1")
        command = get_command(run, "spin", threads=4)
        self.assertEqual(command[:2], ["numactl", "--membind=1"])
        self.assertIn("-membind-node1-", get_data_file_name(run, "spin", 0, threads=4))
        self.assertFalse(run.hardware_cxl)

    def test_lock_placement_uses_the_hardware_cxl_build(self):
        run = self.run_for("memory.placement=locks", "memory.node=2")
        self.assertNotEqual(get_command(run, "spin", threads=4)[0], "numactl")
        self.assertIn("-cxl-hw-node2-", get_data_file_name(run, "spin", 0, threads=4))
        self.assertTrue(run.hardware_cxl)

    def test_no_placement_leaves_commands_and_names_unchanged(self):
        run = self.run_for()
        self.assertNotEqual(get_command(run, "spin", threads=4)[0], "numactl")
        self.assertNotIn("node", Path(get_data_file_name(run, "spin", 0, threads=4)).name)


class EntryPointTests(unittest.TestCase):
    def test_print_config_applies_overrides_without_running(self):
        result = subprocess.run(
            [sys.executable, "-m", "scripts", "run", "experiments/max_threads.toml",
             "--print-config", "-o", "seconds=9", "-o", "locks.exclude=knuth"],
            cwd=ROOT, capture_output=True, text=True, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("seconds = 9.0", result.stdout)
        self.assertIn('exclude = ["knuth"]', result.stdout)

    def test_bad_override_exits_with_usage_error(self):
        result = subprocess.run(
            [sys.executable, "-m", "scripts", "run", "-o", "bench=nope", "--print-config"],
            cwd=ROOT, capture_output=True, text=True, check=False,
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("`bench` must be one of", result.stderr)


if __name__ == "__main__":
    unittest.main()
