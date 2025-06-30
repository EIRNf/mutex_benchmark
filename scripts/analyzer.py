from .constants import *
from .plotter import plot_one_cdf, finish_plotting_cdf

import pandas as pd

def analyze(data):
    # TODO add more stuff here
    output = ""
    output += "\n"
    for mutex_name in Constants.mutex_names:
        output += f"Mutex {mutex_name:>8} average time: {data[mutex_name]['Time Spent'].mean():.7f} standard deviation: {data[mutex_name]['Time Spent'].std():.7f}\n"
        plot_one_cdf(
            data[mutex_name]["Time Spent"], 
            mutex_name,
            xlabel="Thread time (seconds)",
            ylabel="% of threads under",
            title=f"{mutex_name}",
            skip=0,
            # worst_case=worst_case
        )
    finish_plotting_cdf("Thread time")
    return output

def analyze_lock_level(data):
    # TODO add more stuff here
    output = ""
    output += "\n"
    for mutex_name in Constants.mutex_names:
        output += f"Mutex {mutex_name:>8} average time: {data[mutex_name]['Time Spent'].mean():.7f} standard deviation: {data[mutex_name]['Time Spent'].std():.7f}\n"
        plot_one_cdf(
            data[mutex_name]["Time Spent"], 
            mutex_name,
            xlabel="Lock time (seconds)",
            ylabel="% of iterations under",
            title=f"{mutex_name}",
            skip=0,
            # worst_case=worst_case
        )
    finish_plotting_cdf("Lock time")
    return output
