from .constants import *
from .plotter import plot_one_cdf, plot_one_graph, finish_plotting_cdf, finish_plotting_graph

import pandas as pd
import numpy as np
import matplotlib.pyplot as plt

def analyze(data):
    # TODO add more stuff here
    output = ""
    output += "\n"
    for mutex_name in Constants.mutex_names:
        output += f"Mutex {mutex_name:>8} average iterations: {data[mutex_name]['# Iterations'].mean():.7f} standard deviation: {data[mutex_name]['# Iterations'].std():.7f}\n"
        plot_one_cdf(
            data[mutex_name]["# Iterations"], 
            mutex_name,
            xlabel="Thread iterations (counts)",
            ylabel="% of threads under",
            title=f"{mutex_name}",
            # worst_case=worst_case
        )
    finish_plotting_cdf("Thread iterations")
    return output

def analyze_lock_level(data):
    # TODO add more stuff here
    output = ""
    output += "\n"
    for mutex_name in Constants.mutex_names:
        output += f"Mutex {mutex_name:>8} average time: {data[mutex_name]['Time Spent'].mean():.9f} standard deviation: {data[mutex_name]['Time Spent'].std():.9f}\n"
        plot_one_cdf(
            data[mutex_name]["Time Spent"], 
            mutex_name,
            xlabel="Lock time (seconds)",
            ylabel="% of iterations under",
            title=f"{mutex_name}",
            average_lock_time=data[mutex_name]['Time Spent'].mean(),
        )
    finish_plotting_cdf("Lock time")
    return output

def analyze_iter_v_threads(data):
    output=""
    output +="\n"
    figure, axis = plt.subplots(1, 2)
    for mutex_name in Constants.mutex_names:
        # output += f"Mutex {mutex_name:>8} average time: {np.array(data[mutex_name]).mean():.7f} standard deviation: {np.array(data[mutex_name]).mean():.7f}\n"
        mean_values = [thread.mean() for thread in data[mutex_name]]
        print(mean_values)
        plot_one_graph(
            axis[0],
            np.array(range(Constants.threads_start, Constants.threads_end, Constants.threads_step)),
            mean_values,
            mutex_name,
            xlabel="# of Threads",
            ylabel="# Iterations",
            title=f"{mutex_name}",
            skip=-1,
        )

        stdev_values = [np.std(thread) for thread in data[mutex_name]]
        plot_one_graph(
            axis[1],
            np.array(range(Constants.threads_start, Constants.threads_end, Constants.threads_step)),
            stdev_values,
            mutex_name,
            xlabel="# of Threads",
            ylabel="# Iterations Standard Deviation",
            title=f"{mutex_name}",
            skip=-1
        )

        
    finish_plotting_graph(axis)
    return output
