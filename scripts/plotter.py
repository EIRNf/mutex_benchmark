import pandas as pd # pyright: ignore[reportMissingModuleSource]
import numpy as np # pyright: ignore[reportMissingImports]
import matplotlib.pyplot as plt # pyright: ignore[reportMissingModuleSource]
import seaborn as sns # pyright: ignore[reportMissingModuleSource]
import itertools

from math import ceil

from .constants import Constants
from .logger import logger

# Color Changer/Symbols.
MARKERS = itertools.cycle(['o', 's', '^', 'D', '*', 'X', 'v', '<', '>', 'P', 'H'])
coloring = [
    "#e6194b", "#3cb44b", "#ffe119", "#4363d8", "#f58231",
    "#911eb4", "#46f0f0", "#f032e6", "#bcf60c", "#fabebe",
    "#008080", "#e6beff", "#9a6324", "#fffac8", "#800000",
    "#aaffc3", "#808000", "#ffd8b1", "#000075", "#808080",
    "#ffffff", "#000000", "#ff4500", "#00ced1", "#ff69b4",
    "#1e90ff", "#7cfc00", "#ff1493", "#00ff7f", "#dc143c",
    "#8a2be2", "#00bfff", "#ff6347", "#7fffd4", "#d2691e",
    "#6495ed", "#dda0dd", "#f0e68c", "#ffb6c1", "#a52a2a"
]
COLORS = itertools.cycle(coloring)
MUTEX_STYLES = {}

def get_style(mutex_name):
    if mutex_name not in MUTEX_STYLES:
        MUTEX_STYLES[mutex_name] = {
            "marker": next(MARKERS),
            "color": next(COLORS)
        }
    return MUTEX_STYLES[mutex_name]


def get_savefig_filepath():
    from os.path import isfile
    if Constants.iter:
        extension = "iter"
    else:
        extension = "cdf"
    triplet = f"{Constants.bench_n_threads}_{Constants.bench_n_seconds}_{Constants.n_program_iterations}"
    name_base = f"{Constants.data_folder}/../figs/fig-{triplet}-{extension}"
    n = 0
    while isfile(f"{name_base}{n}.png"):
        n += 1
    return f"{name_base}{n}.png"

def fix_legend_point_size(axes=None):
    if axes is None:
        axes = [plt]
    for ax in axes:
        legend = ax.legend(loc='center left', bbox_to_anchor=(1, 0.5), fontsize='small')
        for handle in legend.legend_handles: # type: ignore
            handle._sizes = [30]

def get_cdf_title():
    title = f"Lock time CDF for {Constants.bench_n_threads} threads, {Constants.bench_n_seconds}s ({Constants.n_program_iterations}×)"
    if Constants.noncritical_delay != -1:
        title += f"\nNoncritical delay: {Constants.noncritical_delay:,} ns ({Constants.noncritical_delay:.2e} ns)"
    if Constants.low_contention:
        title += f"\nLow-contention mode: stagger {Constants.stagger_ms} ms/start"
    return title

def display(axis=None, tight_layout=True):
    fix_legend_point_size(axis)
    if tight_layout:
        plt.tight_layout()
    plt.savefig(get_savefig_filepath())
    plt.show()

def plot_one_cdf(series, mutex_name, *, xlabel, ylabel, title, average_lock_time=None):
    logger.info(f"Plotting {mutex_name=}")

    # The y-values should go up from 0 to 1, while the X-values vary along the series
    
    x_values = series.sort_values().reset_index(drop=True)
    y_values = [a/x_values.size for a in range(x_values.size)]
    title += f" ({x_values.size:,} datapoints)"
    if average_lock_time:
        title += f" ({average_lock_time=:.2e})"

    # Skip some values to save time
    logger.info(x_values.size)
    skip = int(ceil(x_values.size / Constants.max_n_points))
    x = [x_values[i] for i in range(0, x_values.size, skip)]
    y = [y_values[i] for i in range(0, x_values.size, skip)]

    if x_values.size == 0:
        logger.error(f"Failed to plot {mutex_name}: No data.")
        return

    y_values = np.linspace(0, 1, x_values.size)

    if average_lock_time:
        title += f" (avg={average_lock_time:.2e})"
    title += f" ({x_values.size:,} datapoints)"

    # Subsample if too many points
    skip = max(1, int(ceil(x_values.size / Constants.max_n_points)))
    x = x_values[::skip]
    y = y_values[::skip]

    style = get_style(mutex_name)

    # No markers for CDF
    if Constants.scatter:
        plt.scatter(x, y, label=title, s=0.2)
    else:
        plt.plot(
            x, y,
            label=title,
            color=style["color"],
            linewidth=0.8,
            alpha=0.9
        )

    plt.xscale("log")
    plt.xlabel(xlabel)
    plt.ylabel(ylabel)

def plot_lock_level(data):
    for mutex_name, df in data:
        logger.info(f"Mutex {mutex_name:<24} average time: {df['Time Spent'].mean():.9f} standard deviation: {df['Time Spent'].std():.9f}")
        plot_one_cdf(
            df["Time Spent"], 
            mutex_name,
            xlabel="Lock time (seconds)",
            ylabel="% of iterations under",
            title=f"{mutex_name}",
            average_lock_time=df['Time Spent'].mean(),
        )
    plt.title(get_cdf_title())
    if Constants.log_scale and not Constants.iter:
        plt.xscale('log')
    display()

def plot_iter(data):
    for mutex_name, df in data:
        logger.info(f"lineplot_with_std: Plotting {mutex_name=}")
        style = get_style(mutex_name)
        sns.lineplot(
            df, 
            x=Constants.iter_variable_name, 
            y="# Iterations", 
            errorbar=("sd", Constants.stdev_scale), 
            label=mutex_name,
            marker=style["marker"],
            color=style["color"]
        )
    plt.yscale("log")
    display()

def plot_iter_rusage(data):
    _, axis = plt.subplots(1, 2)
    for mutex_name, df in data:
        logger.debug(f"Mutex {mutex_name:<24} average time: {np.array(df).mean():.7f} standard deviation: {np.array(df).mean():.7f}")
        for ax, yname, ylabel in zip(axis, ['utime', 'stime'], ["User time", "System time"]):
            plot_one_graph(
                ax,
                df['threads'],
                df[yname],
                mutex_name,
                xlabel=Constants.iter_variable_name,
                ylabel=ylabel,
            )
    axis[0].set_title(f"# User time v threads for {Constants.bench_n_seconds} seconds ({Constants.n_program_iterations}x)")
    axis[1].set_title(f"System time v threads for {Constants.bench_n_seconds} seconds ({Constants.n_program_iterations}x)")
    display(axis, tight_layout=False)

def plot_one_graph(ax, x, y, mutex_name, *, xlabel, ylabel):
    logger.info(f"plot_one_graph: Plotting {mutex_name=}")
    if Constants.scatter:
        ax.scatter(x, y, label=mutex_name, s=0.2)
    else:
        ax.plot(x, y, label=mutex_name)
    ax.set_xlabel(xlabel)
    ax.set_ylabel(ylabel)

    if Constants.log_scale:
        ax.set_yscale("log")
    else:
        ax.set_yscale("linear")
    ax.grid(True, linestyle='--', linewidth=0.5)

