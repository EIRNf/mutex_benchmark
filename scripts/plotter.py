from .constants import *
from .logger import logger

import matplotlib.pyplot as plt

def finish_plotting_cdf(thread_time_or_lock_time):
    print("Finishing plotting...")
    plt.title(f"{thread_time_or_lock_time} CDF for {Constants.bench_n_threads} threads and {Constants.bench_n_iterations} iterations ({Constants.n_program_iterations}x)")
    plt.xscale('log')
    legend = plt.legend()
    for handle in legend.legend_handles:
        handle._sizes = [30]
    plt.show()

def plot_one_cdf(series, mutex_name, xlabel="", ylabel="", title="", skip=-1, worst_case=-1):
    if skip == -1:
        skip = Constants.skip
    logger.info(f"Plotting {mutex_name=}")
    # The y-values should go up from 0 to 1, while the X-values vary along the series
    x_values = series.sort_values().reset_index(drop=True)
    y_values = [a/x_values.size for a in range(x_values.size)]
    # Skip some values to save time
    x = [x_values[i] for i in range(0, x_values.size, 1 + skip)]
    y = [y_values[i] for i in range(0, x_values.size, 1 + skip)]

    if Constants.scatter:
        plt.scatter(x, y, label=title, s=0.2)
    else:
        plt.plot(x, y, label=title)
    plt.xlabel(xlabel)
    plt.ylabel(ylabel)