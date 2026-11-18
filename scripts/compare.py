"""Compare the runs of a suite.

    python -m scripts compare SUITE_DIR [--by environment|experiment] [--baseline NAME] [--only GLOB]

By environment (the default when the suite has environments), each
experiment is compared across environments: DRAM vs CXL for the same lock.
By experiment, the experiments are compared within each environment, for
example Tigon's partition ratios. Each comparison group gets:

  <group>-throughput.png      one panel per lock, one line per series
                              (mean +/- stdev over repetitions)
  <group>-vs-<baseline>.png   per series: mean throughput / the baseline's,
                              per lock and sweep point
  <group>.csv                 the numbers behind both

and across groups:

  summary-vs-<baseline>.png   per series: geometric mean of those ratios over
  summary.csv                 the sweep, per lock and group

Throughput is the per-run total over all threads (as in the run plots)
divided by the run's seconds, so runs of different lengths compare fairly.
Only throughput sweeps are compared; other runs are listed as skipped.
"""

from __future__ import annotations

import math
import os
from dataclasses import dataclass, field

import numpy as np
import pandas as pd  # pyright: ignore[reportMissingModuleSource]

from .config import CONFIG_FILE, ConfigError, ExperimentConfig, RunPaths, load_config
from .rundir import read_json, read_record
from .settings import resolve_run
from .suite import SUITE_RECORD, run_matches

BY_CHOICES = ("environment", "experiment")
_NO_ENVIRONMENT_GROUP = "all"


@dataclass
class Series:
    label: str                 # the environment or experiment being compared
    key: str                   # run key in the suite
    cfg: ExperimentConfig
    status: str
    data: pd.DataFrame         # lock, x, rep, throughput


@dataclass
class Group:
    name: str                  # the experiment (or environment) held fixed
    var: str                   # sweep variable shared by every series
    series: list[Series] = field(default_factory=list)

    @property
    def labels(self) -> list[str]:
        return [s.label for s in self.series]


@dataclass
class Comparison:
    by: str
    baseline: str
    groups: list[Group]
    stats: dict[str, pd.DataFrame]       # group name -> per (series, lock, x) stats
    summary: pd.DataFrame                # series, group, lock, geomean_ratio
    notes: list[str]
    written: list[str] = field(default_factory=list)


# --------------------------------------------------------------------------
# Loading
# --------------------------------------------------------------------------

def run_seconds(cfg: ExperimentConfig) -> float:
    """The measured duration of one benchmark run (Tigon runs whole seconds)."""
    return float(max(1, int(cfg.seconds))) if cfg.is_tigon else cfg.seconds


def load_throughput(run_dir: str, cfg: ExperimentConfig) -> pd.DataFrame:
    """Per-repetition throughput of one run: rows of lock, x, rep, throughput.

    Points that were never written (hung or still pending) are simply absent.
    """
    from .dataloader import load_data_iter

    run = resolve_run(cfg, RunPaths.for_existing_run(run_dir))
    var = cfg.sweep.var
    frames = []
    for lock, df in load_data_iter(run):
        totals = df.groupby([var, "run"], as_index=False)["# Iterations"].sum()
        frames.append(pd.DataFrame({
            "lock": lock,
            "x": totals[var].astype(int),
            "rep": totals["run"].astype(int),
            "throughput": totals["# Iterations"] / run_seconds(cfg),
        }))
    if not frames:
        return pd.DataFrame({"lock": [], "x": [], "rep": [], "throughput": []})
    return pd.concat(frames, ignore_index=True)


def _group_runs(suite_dir: str, by: str, only) -> tuple[list[Group], list[str]]:
    record = read_json(os.path.join(suite_dir, SUITE_RECORD))
    if record is None:
        raise ConfigError(f"{suite_dir} has no readable {SUITE_RECORD}; it is not a suite folder")
    environments = [env["name"] for env in record.get("environments", [])]
    if by == "environment" and len(environments) < 2:
        raise ConfigError("this suite has fewer than two environments; compare with --by experiment")

    notes, groups = [], {}
    for entry in record.get("runs", []):
        key, experiment, environment = entry["key"], entry["experiment"], entry.get("environment")
        if only and not run_matches(only, key, experiment, environment):
            continue
        run_dir = os.path.join(suite_dir, *key.split("/"))
        run_record = read_record(run_dir)
        if run_record is None:
            notes.append(f"{key}: skipped, not run yet")
            continue
        cfg = load_config(os.path.join(run_dir, CONFIG_FILE))
        if cfg.capture != "throughput" or cfg.sweep is None:
            notes.append(f"{key}: skipped, only throughput sweeps are compared")
            continue
        status = run_record.get("status", "unknown")
        if status != "complete":
            notes.append(f"{key}: included with the data it wrote (status {status})")
        group_name, label = (
            (experiment, environment) if by == "environment"
            else (environment or _NO_ENVIRONMENT_GROUP, experiment)
        )
        group = groups.setdefault(group_name, Group(group_name, cfg.sweep.var))
        if cfg.sweep.var != group.var:
            notes.append(f"{key}: skipped, sweeps {cfg.sweep.var} while {group_name} sweeps {group.var}")
            continue
        group.series.append(Series(label, key, cfg, status, load_throughput(run_dir, cfg)))

    kept = []
    for group in groups.values():
        if len(group.series) < 2:
            notes.append(f"{group.name}: skipped, needs at least two runs to compare")
        else:
            kept.append(group)
    if not kept:
        raise ConfigError("nothing to compare: " + ("; ".join(notes) or "no runs selected"))
    return kept, notes


# --------------------------------------------------------------------------
# Numbers
# --------------------------------------------------------------------------

def _lock_order(group: Group) -> list[str]:
    order = []
    for series in group.series:
        for lock in series.data["lock"]:
            if lock not in order:
                order.append(lock)
    return order


def group_stats(group: Group, baseline: str | None) -> pd.DataFrame:
    """Mean, stdev and count per (series, lock, x), plus the ratio to the baseline."""
    data = pd.concat([s.data.assign(series=s.label) for s in group.series], ignore_index=True)
    stats = (
        data.groupby(["series", "lock", "x"], sort=False)["throughput"]
        .agg(mean="mean", std="std", n="count")
        .reset_index()
    )
    if baseline in group.labels:
        base = stats[stats["series"] == baseline][["lock", "x", "mean"]].rename(columns={"mean": "base"})
        stats = stats.merge(base, on=["lock", "x"], how="left")
        stats["ratio"] = stats["mean"] / stats["base"].where(stats["base"] > 0)
        stats = stats.drop(columns="base")
    else:
        stats["ratio"] = np.nan
    return stats


def _geomean(values) -> float:
    positive = [v for v in values if np.isfinite(v) and v > 0]
    return math.exp(sum(math.log(v) for v in positive) / len(positive)) if positive else np.nan


def summarize(groups: list[Group], stats: dict[str, pd.DataFrame], baseline: str) -> pd.DataFrame:
    rows = []
    for group in groups:
        frame = stats[group.name]
        for label in group.labels:
            if label == baseline or baseline not in group.labels:
                continue
            for lock, lock_rows in frame[frame["series"] == label].groupby("lock", sort=False):
                rows.append({"series": label, "group": group.name, "lock": lock,
                             "geomean_ratio": _geomean(lock_rows["ratio"])})
    return pd.DataFrame(rows, columns=["series", "group", "lock", "geomean_ratio"])


# --------------------------------------------------------------------------
# Figures
# --------------------------------------------------------------------------

def _pyplot():
    import matplotlib  # pyright: ignore[reportMissingModuleSource]
    matplotlib.use("Agg")  # files only; never opens a window
    import matplotlib.pyplot as plt  # pyright: ignore[reportMissingModuleSource]
    return plt


def _save(fig, path: str, written: list[str]):
    fig.savefig(path, bbox_inches="tight", dpi=150)
    _pyplot().close(fig)
    written.append(path)


def _axis_label(var: str) -> str:
    return var.replace("_", " ")


def plot_throughput(group: Group, stats: pd.DataFrame, by: str, path: str, written: list[str]):
    from math import ceil
    from matplotlib.ticker import EngFormatter, MaxNLocator  # pyright: ignore[reportMissingModuleSource]
    from .plotter import SERIES_COLORS, SERIES_MARKERS

    plt = _pyplot()
    locks = _lock_order(group)
    ncols = min(4, len(locks))
    nrows = ceil(len(locks) / ncols)
    height = 3.1 * nrows + 0.9   # panels plus a fixed header for the title and legend
    fig, axes = plt.subplots(nrows, ncols, figsize=(4.2 * ncols, height), squeeze=False, sharex=True)
    for ax, lock in zip(axes.flat, locks):
        for index, label in enumerate(group.labels):
            rows = stats[(stats["series"] == label) & (stats["lock"] == lock)].sort_values("x")
            if rows.empty:
                continue
            color = SERIES_COLORS[index % len(SERIES_COLORS)]
            spread = rows["std"].fillna(0)
            ax.plot(rows["x"], rows["mean"], color=color, label=label, linewidth=1.6,
                    marker=SERIES_MARKERS[index % len(SERIES_MARKERS)], markersize=4)
            ax.fill_between(rows["x"], rows["mean"] - spread, rows["mean"] + spread,
                            color=color, alpha=0.15, linewidth=0)
        ax.set_title(lock, fontsize=11)
        ax.set_ylim(bottom=0)
        ax.yaxis.set_major_formatter(EngFormatter(sep=""))  # 12M rather than 1.2 x 1e7
        ax.xaxis.set_major_locator(MaxNLocator(integer=True))  # sweeps are whole numbers
        ax.grid(True, linestyle="--", alpha=0.3, linewidth=0.5)
    for index, ax in enumerate(axes.flat):
        if index >= len(locks):
            ax.set_visible(False)
        elif index + ncols >= len(locks):  # lowest panel in its column: label the x axis
            ax.set_xlabel(_axis_label(group.var))
            ax.xaxis.set_tick_params(labelbottom=True)
    for ax in axes[:, 0]:
        ax.set_ylabel("ops/s, all threads")
    # Lay out the panels first: tight_layout would otherwise also make room
    # for the title and legend, on top of the header reserved here.
    fig.tight_layout(rect=(0, 0, 1, 1 - 0.8 / height))
    handles, labels = axes.flat[0].get_legend_handles_labels()
    fig.suptitle(f"{group.name}: throughput by {by} (mean ± stdev over repetitions)",
                 fontsize=13, fontweight="bold", y=1 - 0.08 / height, va="top")
    fig.legend(handles, labels, loc="upper center", ncol=len(labels), frameon=False,
               bbox_to_anchor=(0.5, 1 - 0.4 / height))
    _save(fig, path, written)


_RATIO_TICKS = (0.1, 0.25, 0.5, 0.75, 1, 1.5, 2, 4, 10)


def _ratio_heatmap(ax, table: pd.DataFrame, extent: float, colorbar_label: str):
    """Ratios on a log color scale centred on 1 (yellow); below 1 is red, above green."""
    import seaborn as sns  # pyright: ignore[reportMissingModuleSource]
    from matplotlib.colors import LogNorm  # pyright: ignore[reportMissingModuleSource]
    from matplotlib.ticker import FixedLocator, FuncFormatter, NullFormatter  # pyright: ignore

    finite = table.where(np.isfinite(table))
    annotations = finite.apply(lambda column: column.map(lambda v: f"{v:.2f}" if pd.notna(v) else ""))
    ax.set_facecolor("#d9d9d9")
    sns.heatmap(finite.clip(lower=1 / extent, upper=extent), mask=finite.isna(), annot=annotations,
                fmt="", cmap="RdYlGn", norm=LogNorm(vmin=1 / extent, vmax=extent),
                linewidths=0.5, linecolor="white", ax=ax, cbar_kws={"label": colorbar_label})
    colorbar = ax.collections[0].colorbar
    colorbar.ax.yaxis.set_major_locator(
        FixedLocator([t for t in _RATIO_TICKS if 1 / extent <= t <= extent]))
    colorbar.ax.yaxis.set_major_formatter(FuncFormatter(lambda value, _: f"{value:g}"))
    colorbar.ax.yaxis.set_minor_formatter(NullFormatter())


def _extent(tables) -> float:
    values = pd.concat([t.stack() for t in tables]) if tables else pd.Series(dtype=float)
    values = values[np.isfinite(values) & (values > 0)]
    if values.empty:
        return 2.0
    return max(float(values.max()), 1 / float(values.min()), 1.05)


def plot_ratios(group: Group, stats: pd.DataFrame, baseline: str, path: str, written: list[str]):
    plt = _pyplot()
    locks = _lock_order(group)
    others = [label for label in group.labels if label != baseline]
    tables = {
        label: stats[stats["series"] == label].pivot(index="lock", columns="x", values="ratio")
        .reindex(index=locks).sort_index(axis=1)
        for label in others
    }
    columns = max(len(t.columns) for t in tables.values())
    fig, axes = plt.subplots(len(others), 1, squeeze=False,
                             figsize=(max(8, columns * 0.7 + 3), (len(locks) * 0.38 + 1.4) * len(others)))
    extent = _extent(list(tables.values()))
    for ax, label in zip(axes[:, 0], others):
        _ratio_heatmap(ax, tables[label], extent, f"{label} / {baseline}")
        ax.set_title(f"{group.name}: {label} vs {baseline} (mean throughput ratio; grey = missing)")
        ax.set_xlabel(_axis_label(group.var))
        ax.set_ylabel("lock")
    fig.tight_layout()
    _save(fig, path, written)


def plot_summary(summary: pd.DataFrame, baseline: str, path: str, written: list[str]):
    plt = _pyplot()
    series = list(dict.fromkeys(summary["series"]))
    tables = {}
    for label in series:
        rows = summary[summary["series"] == label]
        tables[label] = rows.pivot(index="lock", columns="group", values="geomean_ratio").reindex(
            index=list(dict.fromkeys(rows["lock"])), columns=list(dict.fromkeys(rows["group"])))
    locks = max(len(t.index) for t in tables.values())
    groups = max(len(t.columns) for t in tables.values())
    fig, axes = plt.subplots(1, len(series), squeeze=False,
                             figsize=((groups * 1.1 + 3.5) * len(series), locks * 0.38 + 2))
    extent = _extent(list(tables.values()))
    for ax, label in zip(axes[0], series):
        _ratio_heatmap(ax, tables[label], extent, f"{label} / {baseline}")
        ax.set_title(f"{label} vs {baseline}\n(geometric mean over the sweep)")
        ax.set_xlabel("")
        ax.set_ylabel("lock")
        ax.tick_params(axis="x", labelrotation=30)
    fig.tight_layout()
    _save(fig, path, written)


# --------------------------------------------------------------------------
# Entry point
# --------------------------------------------------------------------------

def compare_suite(suite_dir: str, *, by: str | None = None, baseline: str | None = None,
                  only=(), out_dir: str | None = None) -> Comparison:
    """Compare a suite's runs and write figures and CSVs to out_dir."""
    record = read_json(os.path.join(suite_dir, SUITE_RECORD)) or {}
    if by is None:
        by = "environment" if len(record.get("environments", [])) >= 2 else "experiment"
    if by not in BY_CHOICES:
        raise ConfigError(f"--by must be one of {', '.join(BY_CHOICES)}")
    groups, notes = _group_runs(suite_dir, by, only)

    labels = list(dict.fromkeys(label for group in groups for label in group.labels))
    if baseline is None:
        baseline = labels[0]
    elif baseline not in labels:
        raise ConfigError(f"--baseline {baseline!r} is not one of the compared {by}s: {', '.join(labels)}")

    stats = {}
    for group in groups:
        stats[group.name] = group_stats(group, baseline)
        if baseline not in group.labels:
            notes.append(f"{group.name}: no {baseline} run, so no ratios")
    summary = summarize(groups, stats, baseline)
    comparison = Comparison(by, baseline, groups, stats, summary, notes)

    out_dir = out_dir or os.path.join(suite_dir, "compare")
    os.makedirs(out_dir, exist_ok=True)
    for group in groups:
        frame = stats[group.name].rename(columns={"x": group.var})
        csv_path = os.path.join(out_dir, f"{group.name}.csv")
        frame.to_csv(csv_path, index=False)
        comparison.written.append(csv_path)
        plot_throughput(group, stats[group.name], by,
                        os.path.join(out_dir, f"{group.name}-throughput.png"), comparison.written)
        if baseline in group.labels:
            plot_ratios(group, stats[group.name], baseline,
                        os.path.join(out_dir, f"{group.name}-vs-{baseline}.png"), comparison.written)
    if not summary.empty:
        summary_path = os.path.join(out_dir, "summary.csv")
        summary.to_csv(summary_path, index=False)
        comparison.written.append(summary_path)
        plot_summary(summary, baseline, os.path.join(out_dir, f"summary-vs-{baseline}.png"),
                     comparison.written)
    return comparison
