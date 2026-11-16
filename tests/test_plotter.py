import unittest
import contextlib
import io
import sys
from unittest.mock import patch

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd

from scripts.constants import Constants
from scripts.args import init_args
from scripts.main import run_experiment_iter
from scripts.plotter import (
    _compute_all_styles,
    _plot_iter_single,
    _relative_throughput,
    get_mutex_style,
    plot_throughput_heatmap,
)


class PlotterTests(unittest.TestCase):
    def setUp(self):
        self.settings = patch.multiple(
            Constants,
            mutex_names=["exp_spin", "ticket"],
            iter_variable_name="threads",
            bench="max",
            bench_n_seconds=1,
            n_program_iterations=2,
            skip_experiment=True,
            skip_plotting=False,
            rusage=False,
            no_heatmap=False,
            heatmap_ref=None,
            speedup_ref=None,
            speedup=False,
            variability=False,
            create=True,
        )
        self.settings.start()
        self.addCleanup(self.settings.stop)
        self.addCleanup(plt.close, "all")
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
        ratios = _relative_throughput(self.data, "exp_spin")
        with patch("scripts.plotter.display"):
            _plot_iter_single(self.data)
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
            plot_throughput_heatmap(self.data, "exp_spin")
        ax = display.call_args.args[0]
        self.assertEqual(display.call_args.kwargs["suffix"], "-heatmap")
        self.assertIn("0.00", [text.get_text() for text in ax.texts])
        self.assertEqual(len(ax.collections[0].get_array().compressed()), 4)

    def test_missing_reference_is_reported_without_substitution(self):
        with self.assertLogs("scripts.logger", level="WARNING") as logs:
            plot_throughput_heatmap(self.data, "missing")
        self.assertIn("missing", logs.output[0])

    def test_styles_distinguish_many_baseline_and_family_members(self):
        Constants.mutex_names = [f"bitonic_lock_{i}" for i in range(80)]
        _compute_all_styles()
        styles = [get_mutex_style(name) for name in Constants.mutex_names]
        self.assertEqual(len(set(styles)), len(styles))
        self.assertEqual(len({style[0] for style in styles[:20]}), 20)
        self.assertGreater(len({style[2] for style in styles[:11]}), 1)

    def test_throughput_default_produces_both_and_honors_reference(self):
        with patch("scripts.main.load_data_iter", return_value=iter(self.data)), \
             patch("scripts.main.plot_iter") as line, \
             patch("scripts.main.plot_throughput_heatmap") as heatmap:
            run_experiment_iter()
            line.assert_called_once_with(self.data)
            heatmap.assert_called_once_with(self.data, "exp_spin")

        Constants.heatmap_ref = "ticket"
        Constants.speedup_ref = "exp_spin"
        with patch("scripts.main.load_data_iter", return_value=iter(self.data)), \
             patch("scripts.main.plot_iter"), \
             patch("scripts.main.plot_throughput_heatmap") as heatmap:
            run_experiment_iter()
            heatmap.assert_called_once_with(self.data, "ticket")

        Constants.heatmap_ref = None
        Constants.speedup_ref = "ticket"
        with patch("scripts.main.load_data_iter", return_value=iter(self.data)), \
             patch("scripts.main.plot_iter"), \
             patch("scripts.main.plot_throughput_heatmap") as heatmap:
            run_experiment_iter()
            heatmap.assert_called_once_with(self.data, "ticket")

    def test_default_reference_fallback_and_heatmap_opt_out(self):
        Constants.mutex_names = ["ticket"]
        with patch("scripts.main.load_data_iter", return_value=iter(self.data[1:])), \
             patch("scripts.main.plot_iter"), \
             patch("scripts.main.plot_throughput_heatmap") as heatmap, \
             self.assertLogs("scripts.logger", level="WARNING"):
            run_experiment_iter()
            heatmap.assert_called_once_with(self.data[1:], "ticket")

        Constants.no_heatmap = True
        with patch("scripts.main.load_data_iter", return_value=iter(self.data)), \
             patch("scripts.main.plot_iter"), \
             patch("scripts.main.plot_throughput_heatmap") as heatmap:
            run_experiment_iter()
            heatmap.assert_not_called()

    def test_cli_heatmap_reference_and_incompatible_options(self):
        argv = ["scripts.main", "4", "1", "2", "--capture", "throughput",
                "--sweep", "threads", "1", "4", "1"]
        with patch.object(sys, "argv", argv + ["--heatmap-ref", "ticket"]):
            init_args()
        self.assertEqual(Constants.heatmap_ref, "ticket")
        for options in (["--heatmap-ref", "ticket", "--no-heatmap"],
                        ["--plot", "none", "--heatmap-ref", "ticket"]):
            with self.subTest(options=options), patch.object(sys, "argv", argv + options), \
                 contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                init_args()


if __name__ == "__main__":
    unittest.main()
