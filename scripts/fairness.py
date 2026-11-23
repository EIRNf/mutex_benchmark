"""Lock fairness, computed and plotted for every run that has per-thread counts.

For one benchmark execution with per-thread acquisition counts x_1..x_n:

  jain             Jain's fairness index, (sum x)^2 / (n * sum x^2): 1 when every
                   thread acquired equally often, 1/n when one thread did all of it.
  slowest_share    the slowest thread's count over the mean count: 1 = fair,
                   0 = a thread starved for the whole measured window.
  reacquire_share  the share of acquisitions that went to the thread that had
                   just released the lock (none for round-robin handoff; about
                   1/n for a random one; near 1 when a lock lets its holder
                   straight back in).

Executions with one thread are trivially fair and left out. Tigon runs (whose
per-thread rows split one commit total evenly) and rusage captures (no
per-thread counts) get no figure. Each figure
(plots/...-fairness.png) shows the median over repetitions as three
heatmaps, locks by sweep value; the CSV beside it holds every execution.
"""

from __future__ import annotations

import os

import numpy as np
import pandas as pd  # pyright: ignore[reportMissingModuleSource]

from .logger import logger

METRICS = (
    ("jain", "Jain's fairness index", "1 = every thread equal; 1/n = one thread did all"),
    ("slowest_share", "Slowest thread / mean", "1 = fair; 0 = a thread starved"),
    ("reacquire_share", "Same-thread reacquire share", "lock went straight back to its last holder"),
)
COLUMNS = ["lock", "x", "rep", "threads", "acquisitions", "jain", "slowest_share", "reacquire_share"]

# Sequential one-hue ramps (light = low, dark = high): blue for the two
# fairness measures, orange for the reacquire share, which measures handoff.
_BLUE = ["#cde2fb", "#9ec5f4", "#6da7ec", "#3987e5", "#256abf", "#184f95", "#0d366b"]
_ORANGE = ["#fbe3d8", "#f6bfa6", "#f19470", "#eb6834", "#c4501f", "#933a14", "#64270c"]
_MISSING = "#e7e6e2"
_INK = "#0b0b0b"
_INK_SECONDARY = "#52514e"
_ANNOTATE_MAX_CELLS = 600


def execution_metrics(counts, reacquires=None) -> dict:
    """Fairness of one execution from its per-thread counts (NaN where undefined)."""
    x = np.asarray(counts, dtype=float)
    n, total = len(x), float(x.sum())
    metrics = {"threads": n, "acquisitions": total,
               "jain": np.nan, "slowest_share": np.nan, "reacquire_share": np.nan}
    if n < 2 or total <= 0:
        return metrics
    metrics["jain"] = total * total / (n * float((x * x).sum()))
    metrics["slowest_share"] = float(x.min()) / (total / n)
    if reacquires is not None and not pd.isna(reacquires):
        metrics["reacquire_share"] = float(reacquires) / total
    return metrics


def thread_level_metrics(data_list, var: str) -> pd.DataFrame:
    """Per-execution fairness from load_data_iter() output: [(lock, frame)]."""
    rows = []
    for lock, df in data_list:
        for (x, rep), point in df.groupby([var, "run"]):
            reacquires = point["Reacquires"].sum() if "Reacquires" in point else None
            rows.append({"lock": lock, "x": x, "rep": rep,
                         **execution_metrics(point["# Iterations"].to_numpy(), reacquires)})
    return pd.DataFrame(rows, columns=COLUMNS)


def lock_level_metrics(run) -> pd.DataFrame:
    """Per-execution fairness of a latency run, from its per-acquisition rows."""
    from .dataloader import read_metadata
    from .runner import get_data_file_name

    rows = []
    threads = run.bench_n_threads
    for lock in run.mutex_names:
        for rep in range(run.n_program_iterations):
            path = get_data_file_name(run, lock, rep)
            if not (os.path.isfile(path) and os.path.getsize(path) > 0):
                continue
            ids = pd.read_csv(path, names=["Thread ID"], usecols=[0], comment="#")["Thread ID"]
            counts = ids.value_counts().reindex(range(threads), fill_value=0).to_numpy()
            metadata = read_metadata(path)
            reacquires = float(metadata["reacquires"]) if "reacquires" in metadata else None
            rows.append({"lock": lock, "x": threads, "rep": rep,
                         **execution_metrics(counts, reacquires)})
    return pd.DataFrame(rows, columns=COLUMNS)


def _ramp(colors):
    from matplotlib.colors import LinearSegmentedColormap
    return LinearSegmentedColormap.from_list("ramp", colors).with_extremes(bad=_MISSING)


def _text_color(value):
    # Dark cells (the upper part of each ramp) take light text.
    return "#ffffff" if value >= 0.55 else _INK


def plot_fairness(run, metrics: pd.DataFrame, x_label: str):
    """Three heatmaps (median over repetitions) and the per-execution CSV."""
    import matplotlib.pyplot as plt  # pyright: ignore[reportMissingModuleSource]
    from .plotter import DPI, _apply_style, describe_workload, get_savefig_filepath

    usable = metrics.dropna(subset=["jain"])
    if usable.empty:
        logger.info("Fairness: no executions with two or more threads; no fairness figure")
        return None
    medians = usable.groupby(["lock", "x"])[["jain", "slowest_share", "reacquire_share"]].median()
    locks = [lock for lock in run.mutex_names if lock in medians.index.get_level_values("lock")]
    xs = sorted(medians.index.get_level_values("x").unique())

    # Runs from before the benchmark counted reacquires get two panels.
    panels = METRICS if usable["reacquire_share"].notna().any() else METRICS[:2]
    _apply_style()
    rows, cols = len(locks), len(xs)
    cell_w = 0.55 if cols > 1 else 1.1
    width = max(8.0 * len(panels) / 2, len(panels) * (cols * cell_w + 1.6) + 2.5)
    height = max(4.0, rows * 0.32 + 2.6)
    fig, axes = plt.subplots(1, len(panels), figsize=(width, height), sharey=True)
    annotate = rows * cols <= _ANNOTATE_MAX_CELLS
    for ax, (column, title, note) in zip(axes, panels):
        grid = medians[column].unstack("x").reindex(index=locks, columns=xs)
        colors = _ORANGE if column == "reacquire_share" else _BLUE
        image = ax.imshow(np.ma.masked_invalid(grid.to_numpy(dtype=float)), cmap=_ramp(colors),
                          vmin=0.0, vmax=1.0, aspect="auto", interpolation="nearest")
        if annotate:
            for r in range(rows):
                for c in range(cols):
                    value = grid.iat[r, c]
                    if pd.notna(value):
                        ax.text(c, r, f"{value:.2f}", ha="center", va="center", fontsize=7,
                                color=_text_color(value))
        ax.set_xticks(range(cols), [str(x) for x in xs], fontsize=8)
        ax.set_yticks(range(rows), locks, fontsize=8)
        ax.set_xlabel(x_label)
        ax.set_title(f"{title}\n{note}", fontsize=10, color=_INK)
        ax.tick_params(length=0)
        for spine in ax.spines.values():
            spine.set_visible(False)
        bar = fig.colorbar(image, ax=ax, fraction=0.05, pad=0.02)
        bar.outline.set_visible(False)
        bar.ax.tick_params(labelsize=8, colors=_INK_SECONDARY)
    reps = int(usable.groupby(["lock", "x"]).size().max())
    heading = (f"{run.bench.upper()} fairness — median of {reps} repetition(s) per cell; "
               "grey = no data; single-thread points left out")
    if len(panels) < len(METRICS):
        heading += "\n(same-thread reacquires were not recorded by this run's benchmark)"
    workload = describe_workload(run)
    fig.suptitle(f"{heading}\n{workload}" if workload else heading, fontsize=11)
    fig.tight_layout()
    png_path = get_savefig_filepath(run, "-fairness")
    fig.savefig(png_path, bbox_inches="tight", dpi=DPI)
    logger.info(f"Saved figure → {png_path}")
    csv_path = os.path.splitext(png_path)[0] + ".csv"
    metrics.to_csv(csv_path, index=False)
    logger.info(f"Saved fairness data → {csv_path}")
    if run.show:
        plt.show()
    else:
        plt.close(fig)
    return png_path


def fairness_for_iter_run(run, data_list):
    """The fairness figure and CSV of a sweep run (throughput capture)."""
    if run.rusage:
        logger.info("Fairness: capture = rusage records no per-thread counts; no fairness figure")
        return None
    if run.bench.startswith("tigon"):
        # Tigon reports one commit total; its per-thread rows split it evenly.
        logger.info("Fairness: Tigon runs have no measured per-thread counts; no fairness figure")
        return None
    metrics = thread_level_metrics(data_list, run.iter_variable_name)
    return plot_fairness(run, metrics, run.iter_variable_name.replace("_", " "))


def fairness_for_lock_level_run(run):
    """The fairness figure and CSV of a latency run (one thread count)."""
    return plot_fairness(run, lock_level_metrics(run), "threads")
