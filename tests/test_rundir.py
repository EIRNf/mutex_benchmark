import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from scripts.builder import verify_build_matches
from scripts.config import (
    ConfigError,
    RunPaths,
    apply_to_constants,
    config_from_toml,
    load_config,
)
from scripts.constants import Constants
from scripts.main import run_configured
from scripts.rundir import expected_data_files

ROOT = Path(__file__).resolve().parents[1]

SWEEP = """
name = "sweep-test"
bench = "max"
capture = "throughput"
seconds = 1
repetitions = 1
[locks]
include = ["spin", "ticket"]
[sweep]
var = "threads"
start = 1
stop = 2
step = 1
[plot]
layout = "faceted"
"""


def write_csv(path, rows=((0, 1.0, 1000), (1, 1.0, 1200))):
    with open(path, "w", encoding="utf-8") as handle:
        handle.writelines(f"{a},{b},{c}\n" for a, b, c in rows)


class RunPathsTests(unittest.TestCase):
    def test_new_run_folder_layout(self):
        with tempfile.TemporaryDirectory() as root:
            paths = RunPaths.for_new_run("demo", root=root)
            self.assertRegex(os.path.basename(paths.run_dir), r"^\d{8}T\d+-demo$")
            self.assertEqual(paths.data, os.path.join(paths.run_dir, "raw"))
            self.assertEqual(paths.plots, os.path.join(paths.run_dir, "plots"))
            self.assertFalse(paths.reuse)

    def test_new_run_refuses_to_overwrite_a_recorded_run(self):
        with tempfile.TemporaryDirectory() as run_dir:
            Path(run_dir, "config.toml").write_text("", encoding="utf-8")
            with self.assertRaisesRegex(ConfigError, "already holds a run"):
                RunPaths.for_new_run("demo", run_dir)

    def test_legacy_reuse_of_default_folder_finds_newest_run_with_data(self):
        with tempfile.TemporaryDirectory() as root:
            for name in ("20260101T000000000001", "20260102T000000000001", "20260103T000000000001", "figs"):
                os.makedirs(os.path.join(root, name))
            write_csv(os.path.join(root, "20260101T000000000001", "a.csv"))
            write_csv(os.path.join(root, "20260102T000000000001", "a.csv"))
            # 20260103 is newer but empty (e.g. an aborted run), so it is skipped.
            paths = RunPaths.legacy(root, "logs", reuse=True, default_root=root)
            self.assertEqual(paths.data, os.path.join(root, "20260102T000000000001"))
            self.assertTrue(paths.reuse)
            self.assertIsNone(paths.run_dir)

    def test_legacy_reuse_without_runs_is_an_error(self):
        with tempfile.TemporaryDirectory() as root:
            with self.assertRaisesRegex(ConfigError, "no previous runs"):
                RunPaths.legacy(root, "logs", reuse=True, default_root=root)

    def test_legacy_new_run_and_explicit_folder(self):
        paths = RunPaths.legacy("data/generated", "data/logs", reuse=False, default_root="data/generated")
        self.assertRegex(paths.data, r"^data/generated/\d{8}T\d+$")
        self.assertEqual(paths.plots, "data/generated/figs")
        self.assertEqual(paths.run_dir, paths.data)  # config.toml sits beside the CSVs
        explicit = RunPaths.legacy("out/raw", "out/logs", reuse=False, default_root="data/generated")
        self.assertEqual((explicit.data, explicit.plots), ("out/raw", "out/figs"))

    def test_existing_run_detects_layout(self):
        with tempfile.TemporaryDirectory() as tmp:
            new_style = os.path.join(tmp, "run")
            os.makedirs(os.path.join(new_style, "raw"))
            Path(new_style, "config.toml").write_text("", encoding="utf-8")
            self.assertEqual(RunPaths.for_existing_run(new_style).data, os.path.join(new_style, "raw"))

            flat = os.path.join(tmp, "flat")
            os.makedirs(flat)
            Path(flat, "config.toml").write_text("", encoding="utf-8")
            paths = RunPaths.for_existing_run(flat)
            self.assertEqual(paths.data, flat)
            self.assertEqual(paths.plots, os.path.join(tmp, "figs"))

            with self.assertRaisesRegex(ConfigError, "has no config.toml"):
                RunPaths.for_existing_run(tmp)


@patch("scripts.main.init_logger", lambda: None)
@patch("scripts.main.build", lambda: None)
@patch("scripts.main.verify_build_matches", lambda: ["-std=c++20"])
class RunRecordTests(unittest.TestCase):
    def start_run(self, tmp):
        cfg = config_from_toml(SWEEP)
        paths = RunPaths.for_new_run(cfg.name, os.path.join(tmp, "run"))
        apply_to_constants(cfg, paths)
        return cfg, paths

    def record(self, paths):
        with open(os.path.join(paths.run_dir, "run.json"), encoding="utf-8") as handle:
            return json.load(handle)

    def test_completed_run_is_self_describing(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg, paths = self.start_run(tmp)
            expected = expected_data_files()
            with patch("scripts.main._dispatch", lambda: write_csv(expected[0])):
                run_configured(cfg)

            self.assertEqual(load_config(os.path.join(paths.run_dir, "config.toml")), cfg)
            record = self.record(paths)
            self.assertEqual(record["status"], "complete")
            self.assertEqual(record["locks"], ["spin", "ticket"])
            self.assertEqual(record["placement"]["mode"], "none")
            self.assertEqual(record["build"]["cpp_args"], ["-std=c++20"])
            self.assertEqual(record["data_points"]["expected"], 4)  # 2 locks x 2 thread counts
            self.assertEqual(record["data_points"]["written"], 1)
            self.assertEqual(len(record["data_points"]["missing"]), 3)
            self.assertIsNotNone(record["finished_utc"])

    def test_failed_run_records_the_error(self):
        def boom():
            raise RuntimeError("benchmark exploded")

        with tempfile.TemporaryDirectory() as tmp:
            cfg, paths = self.start_run(tmp)
            with patch("scripts.main._dispatch", boom), self.assertRaises(RuntimeError):
                run_configured(cfg)
            record = self.record(paths)
            self.assertEqual(record["status"], "failed")
            self.assertIn("benchmark exploded", record["error"])

    def test_interrupted_run_is_marked(self):
        def interrupt():
            raise KeyboardInterrupt

        with tempfile.TemporaryDirectory() as tmp:
            cfg, paths = self.start_run(tmp)
            with patch("scripts.main._dispatch", interrupt), self.assertRaises(KeyboardInterrupt):
                run_configured(cfg)
            self.assertEqual(self.record(paths)["status"], "interrupted")


class BuildVerificationTests(unittest.TestCase):
    def build_dir(self, tmp, cpp_args):
        info = Path(tmp, "meson-info")
        info.mkdir()
        (info / "intro-buildoptions.json").write_text(
            json.dumps([{"name": "cpp_args", "value": cpp_args}]), encoding="utf-8"
        )
        return tmp

    def test_build_must_match_the_placement_mode(self):
        with tempfile.TemporaryDirectory() as tmp:
            build_dir = self.build_dir(tmp, ["-Dhardware_cxl", "-lnuma", "-std=c++20"])
            with patch.multiple(Constants, hardware_cxl=False, placement="none"):
                with self.assertRaisesRegex(RuntimeError, "configured with -Dhardware_cxl"):
                    verify_build_matches(build_dir)
            with patch.multiple(Constants, hardware_cxl=True, placement="locks"):
                self.assertIn("-Dhardware_cxl", verify_build_matches(build_dir))

    def test_unconfigured_build_is_an_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(RuntimeError, "not a configured Meson build"):
                verify_build_matches(tmp)


class PlotCommandTests(unittest.TestCase):
    def run_cli(self, *argv):
        env = {**os.environ, "MPLBACKEND": "Agg"}
        return subprocess.run(
            [sys.executable, "-m", "scripts", *argv],
            cwd=ROOT, env=env, capture_output=True, text=True, check=False,
        )

    def test_replots_a_recorded_run_without_flags(self):
        with tempfile.TemporaryDirectory() as tmp:
            run_dir = os.path.join(tmp, "run")
            cfg = config_from_toml(SWEEP)
            paths = RunPaths.for_new_run(cfg.name, run_dir)
            os.makedirs(paths.data)
            Path(run_dir, "config.toml").write_text(SWEEP, encoding="utf-8")
            apply_to_constants(cfg, paths)
            for path in expected_data_files():
                write_csv(path)

            result = self.run_cli("plot", run_dir, "-o", "plot.layout=combined")
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            plots = os.listdir(os.path.join(run_dir, "plots"))
            self.assertTrue(any(name.endswith(".png") for name in plots), plots)

    def test_only_plot_settings_can_change(self):
        with tempfile.TemporaryDirectory() as run_dir:
            Path(run_dir, "config.toml").write_text(SWEEP, encoding="utf-8")
            result = self.run_cli("plot", run_dir, "-o", "seconds=3")
            self.assertEqual(result.returncode, 2)
            self.assertIn("only plot.* settings can change", result.stderr)

    def test_plot_requires_a_recorded_run(self):
        with tempfile.TemporaryDirectory() as run_dir:
            result = self.run_cli("plot", run_dir)
            self.assertEqual(result.returncode, 2)
            self.assertIn("has no config.toml", result.stderr)


if __name__ == "__main__":
    unittest.main()
