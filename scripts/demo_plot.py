"""
Demo script: generate synthetic benchmark CSV-like data and produce plots to illustrate
ways to visualize dense series. Produces:
 - original_combined_iter.png (many overlapping lines using existing style)
 - median_quantile_iter.png (median + quantile ribbons to reduce clutter)
 - heatmap_iter.png (heatmap of median throughput per mutex vs threads)

Run: python3 -m scripts.demo_plot
"""

import os

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
import seaborn as sns

from .constants import Constants
from .plotter import _apply_style, classify_mutex, get_mutex_style

# Ensure output directories
Constants.data_folder = "./data/generated"
figs_dir = os.path.join(Constants.data_folder, "..", "figs")
os.makedirs(figs_dir, exist_ok=True)

# Synthetic data parameters
n_mutex = 40
threads_range = list(range(1, 65, 4))  # 1,5,9,...,61
n_runs = 5

# Create synthetic mutex names (some prefixes to exercise grouping)
base_names = []
for i in range(n_mutex):
    if i % 6 == 0:
        prefix = "bitonic_"
        suffix = ["cas", "bl", "lamport"][i % 3]
    elif i % 6 == 1:
        prefix = "periodic_"
        suffix = ["cas", "bl"][i % 2]
    elif i % 6 == 2:
        prefix = "tree_"
        suffix = "lamport_elevator"
    elif i % 6 == 3:
        prefix = "linear_"
        suffix = "cas_elevator"
    elif i % 6 == 4:
        prefix = "wf_bitonic_"
        suffix = "cas"
    else:
        prefix = ""
        suffix = f"exp_spin_{i}"
    base_names.append(prefix + suffix)

Constants.mutex_names = base_names.copy()
Constants.iter = True
Constants.iter_variable_name = "threads"
Constants.n_program_iterations = n_runs
Constants.bench_n_seconds = 1
Constants.n_program_iterations = n_runs

# Build synthetic data list for plot_iter: list of (mutex_name, dataframe)
# Each dataframe must contain columns: [iter_variable_name, 'run', '# Iterations']

np.random.seed(1)
data_list = []
for idx, name in enumerate(base_names):
    rows = []
    # Assign a baseline scaling so some mutexes perform better/worse
    baseline = 1000.0 + (idx - n_mutex / 2.0) * 15.0
    slope = 0.8 + (idx % 7) * 0.05  # variability in scaling with threads
    for t in threads_range:
        for run in range(n_runs):
            # Throughput scales roughly linearly with threads but with noise
            mean_value = baseline * (t**0.8) * slope
            value = np.random.lognormal(mean=np.log(mean_value), sigma=0.12)
            rows.append(
                {Constants.iter_variable_name: t, "run": run, "# Iterations": value}
            )
    df = pd.DataFrame(rows)
    data_list.append((name, df))

# Plot 1: many overlapping lines using a combined plot (mimic existing behavior)
_apply_style()
plt.figure(figsize=(14, 8))
ax = plt.gca()
for mutex_name, df in data_list:
    color, ls, marker = get_mutex_style(mutex_name)
    # aggregate total throughput per (threads, run)
    agg = df.groupby([Constants.iter_variable_name, "run"], as_index=False)[
        "# Iterations"
    ].sum()
    # For combined original style, plot every run as a line
    for run_id, sub in agg.groupby("run"):
        x = sub[Constants.iter_variable_name].values
        y = sub["# Iterations"].values
        ax.plot(x, y, color=color, linestyle=ls, linewidth=0.8, alpha=0.6)

ax.set_yscale("log")
ax.set_title("Original combined plot - many overlapping lines")
ax.set_xlabel("Threads")
ax.set_ylabel("Total iterations (log)")
plt.grid(True, linestyle="--", alpha=0.3)
path1 = os.path.join(figs_dir, "original_combined_iter.png")
plt.savefig(path1, bbox_inches="tight", dpi=150)
plt.close()

# Plot 2: median + quantile ribbons per mutex (compress runs into summary)
_apply_style()
plt.figure(figsize=(14, 10))
ax = plt.gca()
for mutex_name, df in data_list:
    color, ls, marker = get_mutex_style(mutex_name)
    # pivot to compute quantiles across runs for each threads value
    grouped = df.groupby([Constants.iter_variable_name, "run"], as_index=False)[
        "# Iterations"
    ].sum()
    pivot = grouped.pivot(
        index=Constants.iter_variable_name, columns="run", values="# Iterations"
    )
    # compute quantiles
    q10 = pivot.quantile(0.10, axis=1)
    q25 = pivot.quantile(0.25, axis=1)
    q50 = pivot.quantile(0.50, axis=1)
    q75 = pivot.quantile(0.75, axis=1)
    q90 = pivot.quantile(0.90, axis=1)
    x = pivot.index.values

    ax.plot(x, q50.values, color=color, linestyle=ls, linewidth=1.5, alpha=0.9)
    ax.fill_between(x, q25.values, q75.values, color=color, alpha=0.18)
    ax.fill_between(x, q10.values, q90.values, color=color, alpha=0.09)

ax.set_yscale("log")
ax.set_title("Median + quantile ribbons (summary of runs)")
ax.set_xlabel("Threads")
ax.set_ylabel("Total iterations (log)")
plt.grid(True, linestyle="--", alpha=0.3)
path2 = os.path.join(figs_dir, "median_quantile_iter.png")
plt.savefig(path2, bbox_inches="tight", dpi=150)
plt.close()

# Plot 3: heatmap of median throughput per mutex vs threads
# Build matrix (mutex x threads) of median values
median_matrix = []
for mutex_name, df in data_list:
    grouped = df.groupby([Constants.iter_variable_name, "run"], as_index=False)[
        "# Iterations"
    ].sum()
    pivot = grouped.pivot(
        index=Constants.iter_variable_name, columns="run", values="# Iterations"
    )
    median = pivot.quantile(0.5, axis=1)
    median_matrix.append(median.values)

median_matrix = np.array(median_matrix)  # shape (n_mutex, n_threads)
# Sort mutexes by median at max threads to make heatmap more interpretable
order = np.argsort(median_matrix[:, -1])[::-1]
sorted_matrix = median_matrix[order]

plt.figure(figsize=(12, 10))
ax = sns.heatmap(
    np.log10(sorted_matrix + 1),
    cmap="viridis",
    xticklabels=threads_range,
    yticklabels=np.array(base_names)[order],
)
ax.set_title("Heatmap (log10 median throughput)")
ax.set_xlabel("Threads")
ax.set_ylabel("Mutex (sorted by throughput)")
path3 = os.path.join(figs_dir, "heatmap_iter.png")
plt.savefig(path3, bbox_inches="tight", dpi=150)
plt.close()

print("Saved demo figures:")
print(" -", path1)
print(" -", path2)
print(" -", path3)
