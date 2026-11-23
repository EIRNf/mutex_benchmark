import unittest
import contextlib
import dataclasses
import io
import sys
from unittest.mock import patch

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd

from scripts.args import init_args
from scripts.config import RunPaths, config_from_data
from scripts.main import run_experiment_iter
from scripts.plotter import (
    _compute_all_styles,
    _plot_iter_single,
    _relative_throughput,
    get_mutex_style,
    plot_throughput_heatmap,
)
from scripts.settings import resolve_run


def plotting_run(**changes):
    """Settings for a two-lock threads sweep that only plots existing data."""
    cfg = config_from_data({
        "bench": "max", "capture": "throughput", "seconds": 1, "repetitions": 2,
        "locks": {"include": ["exp_spin", "ticket"]},
        "sweep": {"var": "threads", "start": 1, "stop": 4, "step": 1},
    })
    paths = RunPaths(data="unused", plots="unused", logs="unused", reuse=True)
    return dataclasses.replace(resolve_run(cfg, paths, skip_build=True), **changes)


class PlotterTests(unittest.TestCase):
    def setUp(self):
        self.run = plotting_run()
        self.addCleanup(plt.close, "all")
        # Every throughput run also writes a fairness figure; keep it out of the tree.
        fairness = patch("scripts.main.fairness_for_iter_run")
        self.fairness = fairness.start()
        self.addCleanup(fairness.stop)
        self.data = [
            ("exp_spin", pd.DataFrame([
                {"threads": 1, "run": 0, "# Iterations": 60},
                {"threads": 1, "run": 0, "# Iterations": 40},
                {"threads": 1, "run": 1, "# Iterations": 140},
                {"threads": 2, "run": 0, "# Iterations": 0},
                {"threads": 4, "run": 0, "# Iterations": 50},
            ])),
            ("ticket", pd.DataFrame([
                {"threads": 1, "run": 0, "# Iterations": 180},
                {"threads": 1, "run": 1, "# Iterations": 300},
                {"threads": 2, "run": 0, "# Iterations": 10},
                {"threads": 3, "run": 0, "# Iterations": 20},
                {"threads": 4, "run": 0, "# Iterations": 0},
            ])),
        ]

    def test_heatmap_ratios_match_line_means_and_preserve_missing_points(self):
        ratios = _relative_throughput(self.data, "exp_spin", "threads")
        with patch("scripts.plotter.display"):
            _plot_iter_single(self.run, self.data)
            lines = plt.gca().get_lines()
            means = {line.get_label(): line.get_ydata()[0] for line in lines}
        self.assertAlmostEqual(ratios.loc["ticket", 1], 2)
        self.assertAlmostEqual(ratios.loc["ticket", 1], means["ticket"] / means["exp_spin"])
        self.assertEqual(ratios.loc["exp_spin", 1], 1)
        self.assertEqual(ratios.loc["ticket", 4], 0)
        self.assertTrue(pd.isna(ratios.loc["ticket", 2]))
        self.assertTrue(pd.isna(ratios.loc["ticket", 3]))
        self.assertTrue(pd.isna(ratios.loc["exp_spin", 3]))

        with patch("scripts.plotter.display") as display:
            plot_throughput_heatmap(self.run, self.data, "exp_spin")
        ax = display.call_args.args[1]
        self.assertEqual(display.call_args.kwargs["suffix"], "-heatmap")
        self.assertIn("0.00", [text.get_text() for text in ax.texts])
        self.assertEqual(len(ax.collections[0].get_array().compressed()), 4)

    def test_missing_reference_is_reported_without_substitution(self):
        with self.assertLogs("scripts.logger", level="WARNING") as logs:
            plot_throughput_heatmap(self.run, self.data, "missing")
        self.assertIn("missing", logs.output[0])

    def test_styles_distinguish_many_baseline_and_family_members(self):
        names = [f"bitonic_lock_{i}" for i in range(80)]
        _compute_all_styles(names)
        styles = [get_mutex_style(name) for name in names]
        self.assertEqual(len(set(styles)), len(styles))
        self.assertEqual(len({style[0] for style in styles[:20]}), 20)
        self.assertGreater(len({style[2] for style in styles[:11]}), 1)

    def test_throughput_default_produces_both_and_honors_reference(self):
        with patch("scripts.main.load_data_iter", return_value=iter(self.data)), \
             patch("scripts.main.plot_iter") as line, \
             patch("scripts.main.plot_throughput_heatmap") as heatmap:
            run_experiment_iter(self.run)
            line.assert_called_once_with(self.run, self.data)
            heatmap.assert_called_once_with(self.run, self.data, "exp_spin")
            self.fairness.assert_called_once_with(self.run, self.data)

        run = dataclasses.replace(self.run, heatmap_ref="ticket", speedup_ref="exp_spin")
        with patch("scripts.main.load_data_iter", return_value=iter(self.data)), \
             patch("scripts.main.plot_iter"), \
             patch("scripts.main.plot_throughput_heatmap") as heatmap:
            run_experiment_iter(run)
            heatmap.assert_called_once_with(run, self.data, "ticket")

        run = dataclasses.replace(self.run, heatmap_ref=None, speedup_ref="ticket")
        with patch("scripts.main.load_data_iter", return_value=iter(self.data)), \
             patch("scripts.main.plot_iter"), \
             patch("scripts.main.plot_throughput_heatmap") as heatmap:
            run_experiment_iter(run)
            heatmap.assert_called_once_with(run, self.data, "ticket")

    def test_default_reference_fallback_and_heatmap_opt_out(self):
        run = dataclasses.replace(self.run, mutex_names=["ticket"])
        with patch("scripts.main.load_data_iter", return_value=iter(self.data[1:])), \
             patch("scripts.main.plot_iter"), \
             patch("scripts.main.plot_throughput_heatmap") as heatmap, \
             self.assertLogs("scripts.logger", level="WARNING"):
            run_experiment_iter(run)
            heatmap.assert_called_once_with(run, self.data[1:], "ticket")

        run = dataclasses.replace(self.run, no_heatmap=True)
        with patch("scripts.main.load_data_iter", return_value=iter(self.data)), \
             patch("scripts.main.plot_iter"), \
             patch("scripts.main.plot_throughput_heatmap") as heatmap:
            run_experiment_iter(run)
            heatmap.assert_not_called()

    def test_cli_heatmap_reference_and_incompatible_options(self):
        argv = ["scripts.main", "4", "1", "2", "--capture", "throughput",
                "--sweep", "threads", "1", "4", "1"]
        with patch.object(sys, "argv", argv + ["--heatmap-ref", "ticket"]):
            run = init_args()
        self.assertEqual(run.heatmap_ref, "ticket")
        for options in (["--heatmap-ref", "ticket", "--no-heatmap"],
                        ["--plot", "none", "--heatmap-ref", "ticket"]):
            with self.subTest(options=options), patch.object(sys, "argv", argv + options), \
                 contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                init_args()


if __name__ == "__main__":
    unittest.main()
