"""Plotting module for mutex benchmark results.

QUICK CUSTOMIZATION GUIDE
─────────────────────────
• Tweak FIGURE_SIZE, DPI, LINE_WIDTH, etc. in the STYLE CONFIG section below.
• SERIES_COLORS, SERIES_MARKERS and SERIES_LINESTYLES distinguish the selected
  locks consistently across panels.
• AUTO_FACET_THRESHOLD: when series count exceeds this, the plot is
  automatically split into grouped subplots. Override with --plot faceted/combined.
"""

import pandas as pd   # pyright: ignore[reportMissingModuleSource]
import numpy as np    # pyright: ignore[reportMissingImports]
import matplotlib      # pyright: ignore[reportMissingModuleSource]
import matplotlib.pyplot as plt  # pyright: ignore[reportMissingModuleSource]
from matplotlib.colors import LogNorm
import seaborn as sns  # pyright: ignore[reportMissingModuleSource]
import os

from math import ceil
from collections import OrderedDict

from .logger import logger
from .runner import placement_tag


# ════════════════════════════════════════════════════════════════
#  STYLE CONFIG — edit these to change every graph at once
# ════════════════════════════════════════════════════════════════

FIGURE_SIZE          = (14, 8)    # single-plot (width, height) in inches
FACETED_FIG_SIZE     = (20, 14)   # faceted subplot figure size
DPI                  = 150        # saved image resolution
FONT_SIZE            = 12
TITLE_FONT_SIZE      = 14
LEGEND_FONT_SIZE     = 8
AXIS_LABEL_SIZE      = 11
LINE_WIDTH           = 2.0
MARKER_SIZE          = 7
GRID_ALPHA           = 0.3
GRID_STYLE           = '--'
ALPHA                = 0.85       # data-line opacity
TIGHT_PAD            = 2.0

# Series count above which faceted subplots are used automatically
AUTO_FACET_THRESHOLD = 10

SERIES_COLORS = [
    '#0072B2', '#D55E00', '#009E73', '#CC79A7', '#E69F00',
    '#56B4E9', '#8C564B', '#6A3D9A', '#008080', '#B22222',
    '#3C5A14', '#E377C2', '#4D4D4D', '#BCBD22', '#1F78B4',
    '#FF7F00', '#33A02C', '#B15928', '#A65628', '#984EA3',
]
SERIES_MARKERS = ['o', 's', '^', 'D', 'P', 'v', 'X', '<', '>', 'h', '*']
SERIES_LINESTYLES = ['-', '--', '-.', ':']


# ════════════════════════════════════════════════════════════════
#  MUTEX CLASSIFICATION
# ════════════════════════════════════════════════════════════════

# Longest prefixes first so wf_bitonic_ matches before bitonic_
_PREFIX_MAP = [
    ('wf_bitonic_',   'WF Bitonic'),
    ('wf_periodic_',  'WF Periodic'),
    ('lw_bitonic_',   'LW Bitonic'),
    ('lw_periodic_',  'LW Periodic'),
    ('seq_bitonic_',  'Seq Bitonic'),
    ('seq_periodic_', 'Seq Periodic'),
    ('bitonic_',      'Bitonic'),
    ('periodic_',     'Periodic'),
    ('tree_',         'Tree Elevator'),
    ('linear_',       'Linear Elevator'),
    ('net_',          'Net Elevator'),
]


def classify_mutex(name):
    """Return (group_name, lock_type, is_nca) for a mutex name.

    >>> classify_mutex('bitonic_cas')
    ('Bitonic', 'cas', False)
    >>> classify_mutex('tree_lamport_elevator_nca')
    ('Tree Elevator', 'lamport', True)
    >>> classify_mutex('exp_spin')
    ('Baseline', 'exp_spin', False)
    """
    is_nca = name.endswith('_nca')
    base = name[:-4] if is_nca else name
    for prefix, group in _PREFIX_MAP:
        if base.startswith(prefix):
            remainder = base[len(prefix):]
            lock_type = remainder.split('_')[0] if remainder else 'default'
            return group, lock_type, is_nca
    return 'Baseline', name, is_nca


def auto_group_mutexes(mutex_names):
    """Group mutex names by topology → OrderedDict[group, [names]]."""
    groups = OrderedDict()
    for name in mutex_names:
        group, _, _ = classify_mutex(name)
        groups.setdefault(group, []).append(name)
    return groups


# ── Style cache ────────────────────────────────────────────────
_STYLE_CACHE = {}


def _compute_all_styles(mutex_names):
    """Build (color, linestyle, marker) for every mutex in *mutex_names*."""
    _STYLE_CACHE.clear()
    for i, name in enumerate(dict.fromkeys(mutex_names)):
        _STYLE_CACHE[name] = (
            SERIES_COLORS[i % len(SERIES_COLORS)],
            SERIES_LINESTYLES[(i // len(SERIES_COLORS)) % len(SERIES_LINESTYLES)],
            SERIES_MARKERS[i % len(SERIES_MARKERS)],
        )


def get_mutex_style(mutex_name):
    """Return (color, linestyle, marker) for *mutex_name* (see _compute_all_styles)."""
    return _STYLE_CACHE.get(mutex_name, ('#333333', '-', 'o'))


# ════════════════════════════════════════════════════════════════
#  FILE SAVING
# ════════════════════════════════════════════════════════════════

def get_savefig_filepath(run, suffix=""):
    from os.path import isfile
    from datetime import datetime

    ts = datetime.now().strftime("%Y%m%d%H%M%S")
    capture = run.capture
    bench = run.bench + run.bench_variant
    alloc = placement_tag(run) or "malloc"

    sweep_part = ""
    if run.iter:
        var = run.iter_variable_name
        r = run.iter_range        # [start, stop+1, step]
        sweep_part = f"-sweep-{var}-{r[0]}to{r[1]-1}s{r[2]}"

    run_part = (
        f"t{run.bench_n_threads}"
        f"-s{run.bench_n_seconds}"
        f"-r{run.n_program_iterations}"
    )

    figs_dir = run.figs_folder
    os.makedirs(figs_dir, exist_ok=True)
    work = f"-{run.work_tag}" if run.work_tag else ""
    base = f"{figs_dir}/{ts}-{bench}-{capture}{sweep_part}-{run_part}-{alloc}{work}"

    base += suffix
    path = f"{base}.png"
    n = 1
    while isfile(path):
        path = f"{base}-{n}.png"
        n += 1
    return path


# ════════════════════════════════════════════════════════════════
#  WORKLOAD DESCRIPTION (every title states it)
# ════════════════════════════════════════════════════════════════

_WORK_VARS = ("critical_ns", "noncritical_ns", "critical_delay", "noncritical_delay")


def format_ns(ns):
    """200 -> '200 ns', 1500 -> '1.5 µs', 2000000 -> '2 ms'."""
    if ns >= 1_000_000:
        return f"{ns / 1e6:g} ms"
    if ns >= 1000:
        return f"{ns / 1000:g} µs"
    return f"{ns:g} ns"


def describe_workload(run):
    """One line naming a max run's added work, pinning and warmup ('' for other benches).

    With no added work the run is the roof: every thread reacquires the lock
    as soon as it releases it.
    """
    if run.bench != "max":
        return ""
    swept = run.iter_variable_name if run.iter and run.iter_variable_name in _WORK_VARS else None
    legacy_ignored = run.capture != "latency"
    work = []
    if swept == "critical_ns":
        work.append("critical-section work swept (x axis)")
    elif run.critical_ns:
        work.append(f"critical section {format_ns(run.critical_ns)}")
    if swept == "noncritical_ns":
        work.append("non-critical work swept (x axis: mean, uniform 0-2x)")
    elif run.noncritical_ns:
        work.append(f"non-critical ≈{format_ns(run.noncritical_ns)} "
                    f"(uniform 0-{format_ns(2 * run.noncritical_ns)})")
    for name, value, label in (("critical_delay", run.critical_delay, "critical"),
                               ("noncritical_delay", run.noncritical_delay, "non-critical")):
        if swept == name or value != -1:
            amount = "swept (x axis)" if swept == name else f"0-{value}"
            if legacy_ignored:
                note = f"legacy {label} delay {amount} had no effect on throughput runs"
                text = f"Roof: no added work ({note})"
                return " · ".join([text, *_pin_and_warmup(run)])
            work.append(f"{label} busy loop {amount} iterations (legacy)")
    text = "Roof: no added work" if not work else "Added work: " + ", ".join(work)
    return " · ".join([text, *_pin_and_warmup(run)])


def _pin_and_warmup(run):
    pin = "unpinned" if run.pin == "none" else f"pinned {run.pin}"
    if run.warmup_seconds is None:
        warmup = "no warmup recorded"
    else:
        warmup = f"warmup {run.warmup_seconds:g} s"
    return [pin, warmup]


def _with_workload(title, run):
    workload = describe_workload(run)
    return f"{title}\n{workload}" if workload else title


# ════════════════════════════════════════════════════════════════
#  DISPLAY HELPERS
# ════════════════════════════════════════════════════════════════

def _apply_style():
    """Set global matplotlib rcParams for consistent look."""
    matplotlib.rcParams.update({
        'font.size':        FONT_SIZE,
        'axes.titlesize':   TITLE_FONT_SIZE,
        'axes.labelsize':   AXIS_LABEL_SIZE,
        'xtick.labelsize':  FONT_SIZE - 1,
        'ytick.labelsize':  FONT_SIZE - 1,
        'legend.fontsize':  LEGEND_FONT_SIZE,
        'figure.dpi':       DPI,
    })


def _style_legend(ax, ncols=None):
    """Attach a compact, readable legend to *ax*."""
    ncols = ncols or 1
    handles, labels = ax.get_legend_handles_labels()
    if not handles:
        return
    legend = ax.legend(
        handles, labels,
        fontsize=LEGEND_FONT_SIZE,
        ncol=ncols,
        loc='best',
        framealpha=0.9,
        edgecolor='#cccccc',
        borderpad=0.6,
        handlelength=2.5,
        markerscale=0.8,
    )
    for h in legend.legend_handles:
        try:
            h._sizes = [MARKER_SIZE * 4]
        except AttributeError:
            pass


def display(run, axes=None, tight_layout=True, suffix=""):
    """Style legends, save figure, and (only if --show was passed) plt.show().

    plt.show() blocks waiting for a GUI event loop. Matplotlib reports
    interactive-looking backends (e.g. macosx) as usable even when there is
    no real foreground session to display a window in (e.g. run from a
    script/CI/background process), so it isn't safe to auto-detect this --
    showing is opt-in via run.show instead. The figure is always saved
    to disk regardless, so no output is lost by not showing it.
    """
    if axes is not None:
        ax_list = axes if hasattr(axes, '__iter__') else [axes]
        for ax in ax_list:
            _style_legend(ax)
    else:
        _style_legend(plt.gca())
    if tight_layout:
        plt.tight_layout(pad=TIGHT_PAD)
    path = get_savefig_filepath(run, suffix)
    plt.savefig(path, bbox_inches='tight', dpi=DPI)
    logger.info(f"Saved figure → {path}")
    if run.show:
        plt.show()
    else:
        plt.close()


# ════════════════════════════════════════════════════════════════
#  CDF PLOTS
# ════════════════════════════════════════════════════════════════

def get_cdf_title(run):
    title = (f"Lock time CDF — {run.bench_n_threads} threads, "
             f"{run.bench_n_seconds}s ({run.n_program_iterations}×)")
    title = _with_workload(title, run)
    if run.low_contention:
        title += f"\nLow-contention: stagger {run.stagger_ms} ms"
    return title


def plot_one_cdf(run, series, mutex_name, *, xlabel, ylabel, title, average_lock_time=None):
    logger.info(f"Plotting CDF for {mutex_name}")
    x_values = series.sort_values().reset_index(drop=True)
    if x_values.size == 0:
        logger.error(f"No data for {mutex_name}")
        return

    y_values = np.linspace(0, 1, x_values.size)
    skip = max(1, int(ceil(x_values.size / run.max_n_points)))
    x = x_values[::skip]
    y = y_values[::skip]

    label = title
    if average_lock_time:
        label += f" (avg={average_lock_time:.2e})"
    label += f" ({x_values.size:,} pts)"

    color, ls, marker = get_mutex_style(mutex_name)
    if run.scatter:
        plt.scatter(x, y, label=label, s=3, color=color, marker=marker)
    else:
        plt.plot(x, y, label=label, color=color, linestyle=ls,
                 marker=marker, markevery=max(1, len(x) // 12),
                 markersize=MARKER_SIZE * 0.7,
                 linewidth=LINE_WIDTH * 0.6, alpha=ALPHA)

    plt.xscale("log")
    plt.xlabel(xlabel)
    plt.ylabel(ylabel)


def plot_lock_level(run, data):
    _apply_style()
    _compute_all_styles(run.mutex_names)
    plt.figure(figsize=FIGURE_SIZE)
    for mutex_name, df in data:
        avg = df['Time Spent'].mean()
        logger.info(f"Mutex {mutex_name:<24} avg={avg:.9f}  std={df['Time Spent'].std():.9f}")
        plot_one_cdf(
            run, df["Time Spent"], mutex_name,
            xlabel="Lock time (seconds)",
            ylabel="Fraction of iterations ≤ x",
            title=mutex_name,
            average_lock_time=avg,
        )
    plt.title(get_cdf_title(run))
    if run.log_scale and not run.iter:
        plt.xscale('log')
    display(run)


# ════════════════════════════════════════════════════════════════
#  ITERATION PLOTS  (throughput vs threads / delay)
# ════════════════════════════════════════════════════════════════

def _aggregate_total_throughput(df, var):
    """Sum per-thread iterations into per-run totals.

    Raw data has one row per thread per run.  We group by
    (iter_variable, run) and sum '# Iterations' so that each
    observation is the *total* work done in a single run —
    not a single thread's share (which hides starvation).
    """
    agg = (df.groupby([var, 'run'], as_index=False)['# Iterations']
             .sum()
             .rename(columns={'# Iterations': 'Iterations'}))
    return agg


def _iter_title(run):
    bench = run.bench.upper()
    return _with_workload(
        f"{bench} contention — total iterations vs "
        f"{run.iter_variable_name.replace('_', ' ')}\n"
        f"({run.bench_n_seconds}s per run, "
        f"{run.n_program_iterations} repetitions)", run)


def plot_iter(run, data):
    """Auto-select single or faceted layout based on series count."""
    _apply_style()
    _compute_all_styles(run.mutex_names)
    data_list = list(data)  # consume generator once

    faceted_flag = run.faceted
    if faceted_flag is True:
        use_faceted = True
    elif faceted_flag is False:
        use_faceted = False
    else:
        use_faceted = len(data_list) > AUTO_FACET_THRESHOLD

    if use_faceted:
        _plot_iter_faceted(run, data_list)
    else:
        _plot_iter_single(run, data_list)


def _plot_iter_single(run, data_list):
    """All series on one axes — best for ≤ ~10 series."""
    fig, ax = plt.subplots(figsize=FIGURE_SIZE)
    for mutex_name, df in data_list:
        logger.info(f"Plotting {mutex_name}")
        color, ls, marker = get_mutex_style(mutex_name)
        agg = _aggregate_total_throughput(df, run.iter_variable_name)
        sns.lineplot(
            data=agg,
            x=run.iter_variable_name,
            y="Iterations",
            errorbar=None,
            label=mutex_name,
            marker=marker,
            color=color,
            linestyle=ls,
            linewidth=LINE_WIDTH,
            markersize=MARKER_SIZE,
            alpha=ALPHA,
            ax=ax,
        )
    ax.set_yscale("log")
    ax.grid(True, linestyle=GRID_STYLE, alpha=GRID_ALPHA, linewidth=0.5)
    ax.set_title(_iter_title(run))
    ax.set_xlabel(run.iter_variable_name.replace('_', ' ').title())
    ax.set_ylabel("Total Iterations (all threads)")
    display(run, ax)


def _plot_iter_faceted(run, data_list):
    """One subplot per mutex family; baselines drawn faintly in every panel."""
    grouped = OrderedDict()
    baseline_entries = []
    for mutex_name, df in data_list:
        group, _, _ = classify_mutex(mutex_name)
        grouped.setdefault(group, []).append((mutex_name, df))
        if group == 'Baseline':
            baseline_entries.append((mutex_name, df))

    n_groups = len(grouped)
    if n_groups == 0:
        return

    ncols = min(3, n_groups)
    nrows = ceil(n_groups / ncols)
    fig, axes = plt.subplots(nrows, ncols, figsize=FACETED_FIG_SIZE,
                             sharex=True, sharey=True, squeeze=False)

    # Consistent y-limits across all panels using aggregated totals
    all_totals = []
    for _, df in data_list:
        agg = _aggregate_total_throughput(df, run.iter_variable_name)
        all_totals.append(agg['Iterations'].values)
    all_totals = np.concatenate(all_totals)
    y_lo = max(1, float(all_totals.min()) * 0.5)
    y_hi = float(all_totals.max()) * 2.0

    for idx, (group_name, members) in enumerate(grouped.items()):
        r, c = divmod(idx, ncols)
        ax = axes[r][c]

        # Baseline reference in non-baseline panels — visible but secondary
        if group_name != 'Baseline':
            for bname, bdf in baseline_entries:
                bagg = _aggregate_total_throughput(bdf, run.iter_variable_name)
                bcolor, bls, bmarker = get_mutex_style(bname)
                bmean = bagg.groupby(run.iter_variable_name)['Iterations'].mean()
                ax.plot(
                    bmean.index, bmean.values,
                    label=f"{bname} (ref)", color=bcolor,
                    linewidth=LINE_WIDTH * 0.9, alpha=0.50,
                    linestyle=bls, marker=bmarker, markersize=MARKER_SIZE * 0.7, zorder=1,
                )
            # Shade the region between min/max baselines for context
            if len(baseline_entries) >= 2:
                baseline_means = []
                for _, bdf in baseline_entries:
                    bagg = _aggregate_total_throughput(bdf, run.iter_variable_name)
                    bmean = bagg.groupby(run.iter_variable_name)['Iterations'].mean()
                    baseline_means.append(bmean)
                lo = pd.concat(baseline_means, axis=1).min(axis=1)
                hi = pd.concat(baseline_means, axis=1).max(axis=1)
                ax.fill_between(lo.index, lo.values, hi.values,
                                color='#cccccc', alpha=0.20, zorder=0,
                                label='baseline range')

        # Group members
        for mutex_name, df in members:
            logger.info(f"[{group_name}] Plotting {mutex_name}")
            color, ls, marker = get_mutex_style(mutex_name)
            agg = _aggregate_total_throughput(df, run.iter_variable_name)
            sns.lineplot(
                data=agg,
                x=run.iter_variable_name, y="Iterations",
                errorbar=None, label=mutex_name,
                marker=marker, color=color, linestyle=ls,
                linewidth=LINE_WIDTH, markersize=MARKER_SIZE, alpha=ALPHA,
                ax=ax,
            )

        ax.set_yscale("log")
        ax.set_ylim(y_lo, y_hi)
        ax.grid(True, linestyle=GRID_STYLE, alpha=GRID_ALPHA, linewidth=0.5)
        ax.set_title(group_name, fontsize=TITLE_FONT_SIZE - 1, fontweight='bold')
        ax.set_xlabel(run.iter_variable_name.replace('_', ' ').title())
        ax.set_ylabel("Total Iterations" if c == 0 else "")

    # Hide unused subplot slots
    for idx in range(n_groups, nrows * ncols):
        r, c = divmod(idx, ncols)
        axes[r][c].set_visible(False)

    fig.suptitle(_iter_title(run), fontsize=TITLE_FONT_SIZE + 1, fontweight='bold')
    display(run, axes.flatten()[:n_groups])


def _relative_throughput(data_list, ref_name, var):
    """Mean per-run total iterations divided by the reference at each point."""
    means = {
        name: _aggregate_total_throughput(df, var).groupby(var)['Iterations'].mean()
        for name, df in data_list
    }
    if ref_name not in means:
        logger.warning("Heatmap reference mutex '%s' has no data; skipping heatmap", ref_name)
        return None
    totals = pd.DataFrame.from_dict(means, orient='index').sort_index(axis=1)
    reference = totals.loc[ref_name]
    return totals.where(totals >= 0).div(reference.where(reference > 0), axis='columns')


def plot_throughput_heatmap(run, data, ref_name):
    """Plot throughput ratios relative to a chosen mutex at each sweep point."""
    _apply_style()
    ratios = _relative_throughput(list(data), ref_name, run.iter_variable_name)
    if ratios is None:
        return
    finite = ratios.where(np.isfinite(ratios))
    positive = finite.where(finite > 0).stack()
    if positive.empty:
        logger.warning("No valid reference throughput for heatmap against '%s'", ref_name)
        return
    extent = max(float(positive.max()), 1 / float(positive.min()), 1.01)
    colors = finite.clip(lower=1 / extent)
    annotations = finite.apply(
        lambda column: column.map(lambda value: f"{value:.2f}" if pd.notna(value) else "")
    )
    fig, ax = plt.subplots(figsize=(max(10, len(ratios.columns) * 0.65),
                                    max(4, len(ratios.index) * 0.42 + 2)))
    ax.set_facecolor('#d9d9d9')
    sns.heatmap(colors, mask=finite.isna(), annot=annotations, fmt='',
                cmap='RdYlGn', norm=LogNorm(vmin=1 / extent, vmax=extent),
                linewidths=0.5, linecolor='white', ax=ax,
                cbar_kws={'label': f'Total iterations / {ref_name} (1 = equal)'})
    ax.set_title(_with_workload(f"Relative throughput vs {ref_name}\n"
                                "Mean run totals; grey = missing or invalid reference", run))
    ax.set_xlabel(run.iter_variable_name.replace('_', ' ').title())
    ax.set_ylabel("Mutex")
    display(run, ax, suffix="-heatmap")


# ════════════════════════════════════════════════════════════════
#  VARIABILITY PLOT  (coefficient of variation vs iter variable)
# ════════════════════════════════════════════════════════════════

def _compute_variability(df, var):
    """Compute per-thread-count variability stats from aggregated run totals.

    Returns a DataFrame with columns:
        iter_variable, mean, std, cv  (cv = std / mean)
    """
    agg = _aggregate_total_throughput(df, var)
    stats = (agg.groupby(var)['Iterations']
                .agg(['mean', 'std'])
                .reset_index())
    stats['cv'] = stats['std'] / stats['mean']
    stats['cv'] = stats['cv'].fillna(0)
    return stats


def _variability_title(run):
    bench = run.bench.upper()
    return _with_workload(
        f"{bench} contention — run-to-run variability vs "
        f"{run.iter_variable_name.replace('_', ' ')}\n"
        f"({run.bench_n_seconds}s per run, "
        f"{run.n_program_iterations} repetitions)", run)


def plot_variability(run, data):
    """Plot coefficient of variation (std/mean) of total throughput.

    Auto-facets the same way as plot_iter.
    """
    _apply_style()
    _compute_all_styles(run.mutex_names)
    data_list = list(data)

    faceted_flag = run.faceted
    if faceted_flag is True:
        use_faceted = True
    elif faceted_flag is False:
        use_faceted = False
    else:
        use_faceted = len(data_list) > AUTO_FACET_THRESHOLD

    if use_faceted:
        _plot_variability_faceted(run, data_list)
    else:
        _plot_variability_single(run, data_list)


def _plot_variability_single(run, data_list):
    fig, ax = plt.subplots(figsize=FIGURE_SIZE)
    var = run.iter_variable_name
    for mutex_name, df in data_list:
        logger.info(f"Variability: Plotting {mutex_name}")
        color, ls, marker = get_mutex_style(mutex_name)
        stats = _compute_variability(df, var)
        ax.plot(
            stats[var], stats['cv'],
            label=mutex_name, color=color, linestyle=ls,
            marker=marker, linewidth=LINE_WIDTH,
            markersize=MARKER_SIZE, alpha=ALPHA,
        )
    ax.set_xlabel(var.replace('_', ' ').title())
    ax.set_ylabel('Coefficient of Variation (σ / μ)')
    ax.set_title(_variability_title(run))
    ax.grid(True, linestyle=GRID_STYLE, alpha=GRID_ALPHA, linewidth=0.5)
    display(run, ax)


def _plot_variability_faceted(run, data_list):
    grouped = OrderedDict()
    baseline_entries = []
    var = run.iter_variable_name
    for mutex_name, df in data_list:
        group, _, _ = classify_mutex(mutex_name)
        grouped.setdefault(group, []).append((mutex_name, df))
        if group == 'Baseline':
            baseline_entries.append((mutex_name, df))

    n_groups = len(grouped)
    if n_groups == 0:
        return

    ncols = min(3, n_groups)
    nrows = ceil(n_groups / ncols)
    fig, axes = plt.subplots(nrows, ncols, figsize=FACETED_FIG_SIZE,
                             sharex=True, sharey=True, squeeze=False)

    for idx, (group_name, members) in enumerate(grouped.items()):
        r, c = divmod(idx, ncols)
        ax = axes[r][c]

        # Baseline reference — visible but secondary
        if group_name != 'Baseline':
            for bname, bdf in baseline_entries:
                bstats = _compute_variability(bdf, var)
                bcolor, bls, bmarker = get_mutex_style(bname)
                ax.plot(
                    bstats[var], bstats['cv'],
                    label=f"{bname} (ref)", color=bcolor,
                    linewidth=LINE_WIDTH * 0.9, alpha=0.50,
                    linestyle=bls, marker=bmarker, markersize=MARKER_SIZE * 0.7, zorder=1,
                )

        for mutex_name, df in members:
            logger.info(f"[{group_name}] Variability: {mutex_name}")
            color, ls, marker = get_mutex_style(mutex_name)
            stats = _compute_variability(df, var)
            ax.plot(
                stats[var], stats['cv'],
                label=mutex_name, color=color, linestyle=ls,
                marker=marker, linewidth=LINE_WIDTH,
                markersize=MARKER_SIZE, alpha=ALPHA,
            )

        ax.grid(True, linestyle=GRID_STYLE, alpha=GRID_ALPHA, linewidth=0.5)
        ax.set_title(group_name, fontsize=TITLE_FONT_SIZE - 1, fontweight='bold')
        ax.set_xlabel(var.replace('_', ' ').title())
        ax.set_ylabel('CV (σ / μ)' if c == 0 else '')

    for idx in range(n_groups, nrows * ncols):
        r, c = divmod(idx, ncols)
        axes[r][c].set_visible(False)

    fig.suptitle(_variability_title(run), fontsize=TITLE_FONT_SIZE + 1, fontweight='bold')
    display(run, axes.flatten()[:n_groups])


# ════════════════════════════════════════════════════════════════
#  RUSAGE PLOTS
# ════════════════════════════════════════════════════════════════

def plot_iter_rusage(run, data):
    _apply_style()
    _compute_all_styles(run.mutex_names)
    fig, axis = plt.subplots(1, 2, figsize=(FIGURE_SIZE[0] * 1.3, FIGURE_SIZE[1]))
    for mutex_name, df in data:
        color, ls, marker = get_mutex_style(mutex_name)
        for ax, yname, ylabel in zip(axis, ['utime', 'stime'], ["User time", "System time"]):
            ax.plot(
                df['threads'], df[yname],
                label=mutex_name,
                color=color, linestyle=ls, marker=marker,
                linewidth=LINE_WIDTH, markersize=MARKER_SIZE, alpha=ALPHA,
            )
            ax.set_xlabel(run.iter_variable_name.replace('_', ' ').title())
            ax.set_ylabel(ylabel)
            if run.log_scale:
                ax.set_yscale("log")
            ax.grid(True, linestyle=GRID_STYLE, alpha=GRID_ALPHA, linewidth=0.5)
    axis[0].set_title(f"User time vs threads ({run.bench_n_seconds}s, {run.n_program_iterations}×)")
    axis[1].set_title(f"System time vs threads ({run.bench_n_seconds}s, {run.n_program_iterations}×)")
    workload = describe_workload(run)
    if workload:
        fig.suptitle(workload)
    display(run, axis, tight_layout=False)


# ════════════════════════════════════════════════════════════════
#  SPEEDUP TABLE
# ════════════════════════════════════════════════════════════════

def print_speedup_table(run, data):
    """Print and save a speedup table relative to a reference mutex.

    Each cell shows  mutex_mean / reference_mean  at a given thread count.
    Values > 1 mean the mutex is *faster* than the reference.
    """
    _compute_all_styles(run.mutex_names)
    ref_name = run.speedup_ref or 'exp_spin'
    var = run.iter_variable_name
    data_list = list(data)

    # Build {mutex_name: {var_value: mean_iterations}}
    means = OrderedDict()
    for mutex_name, df in data_list:
        agg = _aggregate_total_throughput(df, run.iter_variable_name)
        m = agg.groupby(var)['Iterations'].mean()
        means[mutex_name] = m

    if ref_name not in means:
        logger.warning(f"Reference mutex '{ref_name}' not in data. "
                       f"Using first mutex '{next(iter(means))}' instead.")
        ref_name = next(iter(means))

    ref = means[ref_name]
    var_values = sorted(ref.index)

    # ── Console output ─────────────────────────────────────────
    col_w = max(8, max(len(str(v)) for v in var_values) + 2)
    name_w = max(len(n) for n in means) + 1
    header = f"{'mutex':<{name_w}}" + "".join(f"{v:>{col_w}}" for v in var_values)
    sep = "─" * len(header)

    lines = []
    lines.append(f"\nSpeedup vs {ref_name}  (values > 1 = faster than ref)")
    lines.append(sep)
    lines.append(header)
    lines.append(sep)
    for mname, mseries in means.items():
        tag = "*" if mname == ref_name else " "
        cells = []
        for v in var_values:
            ref_val = ref.get(v, float('nan'))
            m_val = mseries.get(v, float('nan'))
            if ref_val and ref_val > 0:
                ratio = m_val / ref_val
                cells.append(f"{ratio:>{col_w}.2f}")
            else:
                cells.append(f"{'n/a':>{col_w}}")
        lines.append(f"{mname:<{name_w}}" + "".join(cells) + f"  {tag}")
    lines.append(sep)
    lines.append(f"  * = reference ({ref_name})")
    lines.append("")

    table_str = "\n".join(lines)
    print(table_str)

    # ── Save to file ───────────────────────────────────────────
    figs_dir = run.figs_folder
    os.makedirs(figs_dir, exist_ok=True)
    from datetime import datetime
    ts = datetime.now().strftime("%Y%m%d%H%M%S")
    triplet = f"{run.bench_n_threads}_{run.bench_n_seconds}_{run.n_program_iterations}"
    variant = run.bench_variant
    txt_path = f"{figs_dir}/{ts}_{triplet}{variant}-speedup.txt"
    csv_path = f"{figs_dir}/{ts}_{triplet}{variant}-speedup.csv"

    with open(txt_path, "w") as f:
        f.write(table_str)
    logger.info(f"Saved speedup table → {txt_path}")

    # Also save as CSV for easy import
    rows = []
    for mname, mseries in means.items():
        row = {"mutex": mname}
        for v in var_values:
            ref_val = ref.get(v, float('nan'))
            m_val = mseries.get(v, float('nan'))
            row[f"{var}={v}"] = round(m_val / ref_val, 4) if ref_val > 0 else None
        rows.append(row)
    pd.DataFrame(rows).to_csv(csv_path, index=False)
    logger.info(f"Saved speedup CSV  → {csv_path}")
