"""Plotting module for mutex benchmark results.

QUICK CUSTOMIZATION GUIDE
─────────────────────────
• Tweak FIGURE_SIZE, DPI, LINE_WIDTH, etc. in the STYLE CONFIG section below.
• LOCK_TYPE_STYLES maps each underlying lock algorithm (cas, bl, lamport …)
  to a (linestyle, marker) pair — gives instant visual decoding in every panel.
• GROUP_PALETTES assigns a color family to each topology (Bitonic, Periodic,
  Tree Elevator …).  Colors within a group are shades of the same hue.
• AUTO_FACET_THRESHOLD: when series count exceeds this, the plot is
  automatically split into grouped subplots.  Override with --faceted / --combined.
"""

import pandas as pd   # pyright: ignore[reportMissingModuleSource]
import numpy as np    # pyright: ignore[reportMissingImports]
import matplotlib      # pyright: ignore[reportMissingModuleSource]
import matplotlib.pyplot as plt  # pyright: ignore[reportMissingModuleSource]
import seaborn as sns  # pyright: ignore[reportMissingModuleSource]
import os

from math import ceil
from collections import OrderedDict

from .constants import Constants
from .logger import logger


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

# ── Lock type → (linestyle, marker) ───────────────────────────
# The "lock type" is the substring after the topology prefix:
#   bitonic_cas  →  cas     tree_bl_elevator  →  bl
LOCK_TYPE_STYLES = {
    'cas':      ('-',                'o'),   # solid,   circle
    'bl':       ('--',               's'),   # dashed,  square
    'lamport':  (':',                '^'),   # dotted,  triangle
    'elevator': ('-.',               'D'),   # dashdot, diamond
    'bakery':   ((0, (3, 1, 1, 1)), 'P'),   # dense dash-dot, plus
}
DEFAULT_LOCK_STYLE = ('-', 'X')
NCA_LINESTYLE      = (0, (5, 2))            # long dashes for _nca variants

# ── Group → color palette ─────────────────────────────────────
# i-th member of a group draws palette[i % len(palette)].
GROUP_PALETTES = {
    'Baseline':        ['#2c3e50', '#7f8c8d', '#34495e', '#95a5a6', '#566573', '#aab7b8'],
    'Linear Elevator': ['#c0392b', '#e74c3c', '#ff6b6b', '#e55039', '#eb4d4b', '#fc5c65'],
    'Tree Elevator':   ['#2471a3', '#2e86c1', '#3498db', '#5dade2', '#85c1e9', '#0984e3'],
    'Net Elevator':    ['#27ae60', '#2ecc71', '#58d68d', '#82e0aa'],
    'Bitonic':         ['#8e44ad', '#9b59b6', '#af7ac5', '#c39bd3', '#d2b4de', '#7d3c98'],
    'Periodic':        ['#d35400', '#e67e22', '#f39c12', '#f8c471', '#fad7a0', '#ca6f1e'],
    'WF Bitonic':      ['#4a148c', '#6a1b9a', '#7b1fa2', '#8e24aa', '#9c27b0', '#ab47bc'],
    'WF Periodic':     ['#004d40', '#00695c', '#00796b', '#00897b', '#009688', '#26a69a'],
    'LW Bitonic':      ['#1a237e', '#283593', '#303f9f', '#3949ab', '#3f51b5', '#5c6bc0'],
    'LW Periodic':     ['#bf360c', '#d84315', '#e64a19', '#f4511e', '#ff5722', '#ff6e40'],
    'Seq Bitonic':     ['#33691e', '#558b2f', '#689f38', '#7cb342', '#8bc34a', '#9ccc65'],
    'Seq Periodic':    ['#4e342e', '#5d4037', '#6d4c41', '#795548', '#8d6e63', '#a1887f'],
}
FALLBACK_PALETTE = [
    '#e6194b', '#3cb44b', '#4363d8', '#f58231', '#911eb4',
    '#42d4f4', '#f032e6', '#bfef45', '#fabed4', '#469990',
    '#dcbeff', '#9A6324', '#800000', '#aaffc3', '#808000',
]


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


def _compute_all_styles():
    """Build (color, linestyle, marker) for every mutex in Constants.mutex_names."""
    _STYLE_CACHE.clear()
    groups = auto_group_mutexes(Constants.mutex_names)
    for group_name, members in groups.items():
        palette = GROUP_PALETTES.get(group_name, FALLBACK_PALETTE)
        for i, name in enumerate(members):
            _, lock_type, is_nca = classify_mutex(name)
            color = palette[i % len(palette)]
            ls, marker = LOCK_TYPE_STYLES.get(lock_type, DEFAULT_LOCK_STYLE)
            if is_nca:
                ls = NCA_LINESTYLE
            _STYLE_CACHE[name] = (color, ls, marker)


def get_mutex_style(mutex_name):
    """Return (color, linestyle, marker) for *mutex_name*."""
    if not _STYLE_CACHE:
        _compute_all_styles()
    return _STYLE_CACHE.get(mutex_name, ('#333333', '-', 'o'))


# ════════════════════════════════════════════════════════════════
#  FILE SAVING
# ════════════════════════════════════════════════════════════════

def get_savefig_filepath():
    from os.path import isfile
    from datetime import datetime
    ts = datetime.now().strftime("%Y%m%d%H%M%S")
    ext = "iter" if Constants.iter else "cdf"
    if Constants.hardware_cxl:
        ext += "_hcxl"
    elif Constants.software_cxl:
        ext += "_scxl"
    else:
        ext += "_local"
    triplet = f"{Constants.bench_n_threads}_{Constants.bench_n_seconds}_{Constants.n_program_iterations}"
    figs_dir = os.path.join(Constants.data_folder, "..", "figs")
    os.makedirs(figs_dir, exist_ok=True)
    base = f"{figs_dir}/{ts}_{triplet}-{ext}"
    n = 0
    while isfile(f"{base}{n}.png"):
        n += 1
    return f"{base}{n}.png"


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


def display(axes=None, tight_layout=True):
    """Style legends, save figure, and (only if --show was passed) plt.show().

    plt.show() blocks waiting for a GUI event loop. Matplotlib reports
    interactive-looking backends (e.g. macosx) as usable even when there is
    no real foreground session to display a window in (e.g. run from a
    script/CI/background process), so it isn't safe to auto-detect this --
    showing is opt-in via Constants.show instead. The figure is always saved
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
    path = get_savefig_filepath()
    plt.savefig(path, bbox_inches='tight', dpi=DPI)
    logger.info(f"Saved figure → {path}")
    if getattr(Constants, 'show', False):
        plt.show()
    else:
        plt.close()


# ════════════════════════════════════════════════════════════════
#  CDF PLOTS
# ════════════════════════════════════════════════════════════════

def get_cdf_title():
    title = (f"Lock time CDF — {Constants.bench_n_threads} threads, "
             f"{Constants.bench_n_seconds}s ({Constants.n_program_iterations}×)")
    if Constants.noncritical_delay != -1:
        title += f"\nNon-critical delay: {Constants.noncritical_delay:,} ns"
    if Constants.low_contention:
        title += f"\nLow-contention: stagger {Constants.stagger_ms} ms"
    return title


def plot_one_cdf(series, mutex_name, *, xlabel, ylabel, title, average_lock_time=None):
    logger.info(f"Plotting CDF for {mutex_name}")
    x_values = series.sort_values().reset_index(drop=True)
    if x_values.size == 0:
        logger.error(f"No data for {mutex_name}")
        return

    y_values = np.linspace(0, 1, x_values.size)
    skip = max(1, int(ceil(x_values.size / Constants.max_n_points)))
    x = x_values[::skip]
    y = y_values[::skip]

    label = title
    if average_lock_time:
        label += f" (avg={average_lock_time:.2e})"
    label += f" ({x_values.size:,} pts)"

    color, ls, _ = get_mutex_style(mutex_name)
    if Constants.scatter:
        plt.scatter(x, y, label=label, s=0.2, color=color)
    else:
        plt.plot(x, y, label=label, color=color, linestyle=ls,
                 linewidth=LINE_WIDTH * 0.6, alpha=ALPHA)

    plt.xscale("log")
    plt.xlabel(xlabel)
    plt.ylabel(ylabel)


def plot_lock_level(data):
    _apply_style()
    _compute_all_styles()
    plt.figure(figsize=FIGURE_SIZE)
    for mutex_name, df in data:
        avg = df['Time Spent'].mean()
        logger.info(f"Mutex {mutex_name:<24} avg={avg:.9f}  std={df['Time Spent'].std():.9f}")
        plot_one_cdf(
            df["Time Spent"], mutex_name,
            xlabel="Lock time (seconds)",
            ylabel="Fraction of iterations ≤ x",
            title=mutex_name,
            average_lock_time=avg,
        )
    plt.title(get_cdf_title())
    if Constants.log_scale and not Constants.iter:
        plt.xscale('log')
    display()


# ════════════════════════════════════════════════════════════════
#  ITERATION PLOTS  (throughput vs threads / delay)
# ════════════════════════════════════════════════════════════════

def _aggregate_total_throughput(df):
    """Sum per-thread iterations into per-run totals.

    Raw data has one row per thread per run.  We group by
    (iter_variable, run) and sum '# Iterations' so that each
    observation is the *total* work done in a single run —
    not a single thread's share (which hides starvation).
    """
    var = Constants.iter_variable_name
    agg = (df.groupby([var, 'run'], as_index=False)['# Iterations']
             .sum()
             .rename(columns={'# Iterations': 'Iterations'}))
    return agg


def _iter_title():
    bench = Constants.bench.upper()
    return (f"{bench} contention — total iterations vs "
            f"{Constants.iter_variable_name.replace('_', ' ')}\n"
            f"({Constants.bench_n_seconds}s per run, "
            f"{Constants.n_program_iterations} repetitions)")


def plot_iter(data):
    """Auto-select single or faceted layout based on series count."""
    _apply_style()
    _compute_all_styles()
    data_list = list(data)  # consume generator once

    faceted_flag = getattr(Constants, 'faceted', None)
    if faceted_flag is True:
        use_faceted = True
    elif faceted_flag is False:
        use_faceted = False
    else:
        use_faceted = len(data_list) > AUTO_FACET_THRESHOLD

    if use_faceted:
        _plot_iter_faceted(data_list)
    else:
        _plot_iter_single(data_list)


def _plot_iter_single(data_list):
    """All series on one axes — best for ≤ ~10 series."""
    fig, ax = plt.subplots(figsize=FIGURE_SIZE)
    for mutex_name, df in data_list:
        logger.info(f"Plotting {mutex_name}")
        color, ls, marker = get_mutex_style(mutex_name)
        agg = _aggregate_total_throughput(df)
        sns.lineplot(
            data=agg,
            x=Constants.iter_variable_name,
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
    ax.set_title(_iter_title())
    ax.set_xlabel(Constants.iter_variable_name.replace('_', ' ').title())
    ax.set_ylabel("Total Iterations (all threads)")
    display(ax)


def _plot_iter_faceted(data_list):
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
        agg = _aggregate_total_throughput(df)
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
                bagg = _aggregate_total_throughput(bdf)
                bcolor, _, _ = get_mutex_style(bname)
                bmean = bagg.groupby(Constants.iter_variable_name)['Iterations'].mean()
                ax.plot(
                    bmean.index, bmean.values,
                    label=f"{bname} (ref)", color=bcolor,
                    linewidth=LINE_WIDTH * 0.9, alpha=0.50,
                    linestyle='-', zorder=1,
                )
            # Shade the region between min/max baselines for context
            if len(baseline_entries) >= 2:
                baseline_means = []
                for _, bdf in baseline_entries:
                    bagg = _aggregate_total_throughput(bdf)
                    bmean = bagg.groupby(Constants.iter_variable_name)['Iterations'].mean()
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
            agg = _aggregate_total_throughput(df)
            sns.lineplot(
                data=agg,
                x=Constants.iter_variable_name, y="Iterations",
                errorbar=None, label=mutex_name,
                marker=marker, color=color, linestyle=ls,
                linewidth=LINE_WIDTH, markersize=MARKER_SIZE, alpha=ALPHA,
                ax=ax,
            )

        ax.set_yscale("log")
        ax.set_ylim(y_lo, y_hi)
        ax.grid(True, linestyle=GRID_STYLE, alpha=GRID_ALPHA, linewidth=0.5)
        ax.set_title(group_name, fontsize=TITLE_FONT_SIZE - 1, fontweight='bold')
        ax.set_xlabel(Constants.iter_variable_name.replace('_', ' ').title())
        ax.set_ylabel("Total Iterations" if c == 0 else "")

    # Hide unused subplot slots
    for idx in range(n_groups, nrows * ncols):
        r, c = divmod(idx, ncols)
        axes[r][c].set_visible(False)

    fig.suptitle(_iter_title(), fontsize=TITLE_FONT_SIZE + 1, fontweight='bold')
    display(axes.flatten()[:n_groups])


# ════════════════════════════════════════════════════════════════
#  VARIABILITY PLOT  (coefficient of variation vs iter variable)
# ════════════════════════════════════════════════════════════════

def _compute_variability(df):
    """Compute per-thread-count variability stats from aggregated run totals.

    Returns a DataFrame with columns:
        iter_variable, mean, std, cv  (cv = std / mean)
    """
    var = Constants.iter_variable_name
    agg = _aggregate_total_throughput(df)
    stats = (agg.groupby(var)['Iterations']
                .agg(['mean', 'std'])
                .reset_index())
    stats['cv'] = stats['std'] / stats['mean']
    stats['cv'] = stats['cv'].fillna(0)
    return stats


def _variability_title():
    bench = Constants.bench.upper()
    return (f"{bench} contention — run-to-run variability vs "
            f"{Constants.iter_variable_name.replace('_', ' ')}\n"
            f"({Constants.bench_n_seconds}s per run, "
            f"{Constants.n_program_iterations} repetitions)")


def plot_variability(data):
    """Plot coefficient of variation (std/mean) of total throughput.

    Auto-facets the same way as plot_iter.
    """
    _apply_style()
    _compute_all_styles()
    data_list = list(data)

    faceted_flag = getattr(Constants, 'faceted', None)
    if faceted_flag is True:
        use_faceted = True
    elif faceted_flag is False:
        use_faceted = False
    else:
        use_faceted = len(data_list) > AUTO_FACET_THRESHOLD

    if use_faceted:
        _plot_variability_faceted(data_list)
    else:
        _plot_variability_single(data_list)


def _plot_variability_single(data_list):
    fig, ax = plt.subplots(figsize=FIGURE_SIZE)
    var = Constants.iter_variable_name
    for mutex_name, df in data_list:
        logger.info(f"Variability: Plotting {mutex_name}")
        color, ls, marker = get_mutex_style(mutex_name)
        stats = _compute_variability(df)
        ax.plot(
            stats[var], stats['cv'],
            label=mutex_name, color=color, linestyle=ls,
            marker=marker, linewidth=LINE_WIDTH,
            markersize=MARKER_SIZE, alpha=ALPHA,
        )
    ax.set_xlabel(var.replace('_', ' ').title())
    ax.set_ylabel('Coefficient of Variation (σ / μ)')
    ax.set_title(_variability_title())
    ax.grid(True, linestyle=GRID_STYLE, alpha=GRID_ALPHA, linewidth=0.5)
    display(ax)


def _plot_variability_faceted(data_list):
    grouped = OrderedDict()
    baseline_entries = []
    var = Constants.iter_variable_name
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
                bstats = _compute_variability(bdf)
                bcolor, _, _ = get_mutex_style(bname)
                ax.plot(
                    bstats[var], bstats['cv'],
                    label=f"{bname} (ref)", color=bcolor,
                    linewidth=LINE_WIDTH * 0.9, alpha=0.50,
                    linestyle='-', zorder=1,
                )

        for mutex_name, df in members:
            logger.info(f"[{group_name}] Variability: {mutex_name}")
            color, ls, marker = get_mutex_style(mutex_name)
            stats = _compute_variability(df)
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

    fig.suptitle(_variability_title(), fontsize=TITLE_FONT_SIZE + 1, fontweight='bold')
    display(axes.flatten()[:n_groups])


# ════════════════════════════════════════════════════════════════
#  RUSAGE PLOTS
# ════════════════════════════════════════════════════════════════

def plot_iter_rusage(data):
    _apply_style()
    _compute_all_styles()
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
            ax.set_xlabel(Constants.iter_variable_name.replace('_', ' ').title())
            ax.set_ylabel(ylabel)
            if Constants.log_scale:
                ax.set_yscale("log")
            ax.grid(True, linestyle=GRID_STYLE, alpha=GRID_ALPHA, linewidth=0.5)
    axis[0].set_title(f"User time vs threads ({Constants.bench_n_seconds}s, {Constants.n_program_iterations}×)")
    axis[1].set_title(f"System time vs threads ({Constants.bench_n_seconds}s, {Constants.n_program_iterations}×)")
    display(axis, tight_layout=False)


# ════════════════════════════════════════════════════════════════
#  SPEEDUP TABLE
# ════════════════════════════════════════════════════════════════

def print_speedup_table(data):
    """Print and save a speedup table relative to a reference mutex.

    Each cell shows  mutex_mean / reference_mean  at a given thread count.
    Values > 1 mean the mutex is *faster* than the reference.
    """
    _compute_all_styles()
    ref_name = getattr(Constants, 'speedup_ref', None) or 'exp_spin'
    var = Constants.iter_variable_name
    data_list = list(data)

    # Build {mutex_name: {var_value: mean_iterations}}
    means = OrderedDict()
    for mutex_name, df in data_list:
        agg = _aggregate_total_throughput(df)
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
    figs_dir = os.path.join(Constants.data_folder, "..", "figs")
    os.makedirs(figs_dir, exist_ok=True)
    from datetime import datetime
    ts = datetime.now().strftime("%Y%m%d%H%M%S")
    triplet = f"{Constants.bench_n_threads}_{Constants.bench_n_seconds}_{Constants.n_program_iterations}"
    txt_path = f"{figs_dir}/{ts}_{triplet}-speedup.txt"
    csv_path = f"{figs_dir}/{ts}_{triplet}-speedup.csv"

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

