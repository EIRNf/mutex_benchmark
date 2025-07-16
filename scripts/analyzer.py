from .constants import *
from .plotter import plot_one_cdf, plot_one_graph, finish_plotting_cdf, finish_plotting_graph
from .logger import logger

import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
import seaborn as sns

def analyze(data):
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
        )
    finish_plotting_cdf("Thread iterations")
    return output

def analyze_lock_level(data):
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

# def analyze_iter_double_graph(data, iter_variable_name, iter_range):
#     if Constants.iter_threads is None:
#         raise ValueError()
    
#     output=""
#     output +="\n"
#     figure, axis = plt.subplots(1, 2)
#     for mutex_name in Constants.mutex_names:
#         logger.debug(f"{data[mutex_name]['Time Spent']=}")
#         # TODO rename column to # Iterations so that it makes sense
#         mean_values = [x.mean() for x in data[mutex_name]["Time Spent"]]
#         print(mean_values)
#         plot_one_graph(
#             axis[0],
#             np.array(range(*iter_range)),
#             mean_values,
#             mutex_name,
#             xlabel=iter_variable_name,
#             ylabel="# Iterations",
#             title=f"{mutex_name}",
#             skip=-1,
#         )

#         stdev_values = [np.std(x)/x.mean() for x in data[mutex_name]["Time Spent"]]
#         plot_one_graph(
#             axis[1],
#             np.array(range(*iter_range)),
#             stdev_values,
#             mutex_name,
#             xlabel=iter_variable_name,
#             ylabel="# Iterations Standard Deviation",
#             title=f"{mutex_name}",
#             skip=-1
#         )
        
#     finish_plotting_graph(axis)
#     return output

def analyze_iter(data, iter_variable_name, iter_range):
    output=""
    output +="\n"
    axis = plt.axes()
    for mutex_name in Constants.mutex_names:
        # output += f"Mutex {mutex_name:>8} average time: {np.array(data[mutex_name]).mean():.7f} standard deviation: {np.array(data[mutex_name]).mean():.7f}\n"
        # mean_values = [thread.mean() for thread in data[mutex_name]]
        # y_values = np.array(range(*iter_range))
        # df = pd.DataFrame(sum([[y_values[k], ]] for k in range(len(data[mutex_name]))), columns=["# Threads", "Locking time"])
        # print(mean_values)
        plot_one_graph(
            axis,
            np.array(range(*iter_range)),
            None,
            mutex_name,
            error_bars=True,
            xlabel=f"{iter_variable_name.title()}",
            ylabel="Average time spent",
            title=f"{mutex_name}",
            skip=-1,
            data=data[mutex_name],
            iter_variable_name=iter_variable_name,
        )

    legend = plt.legend()
    for handle in legend.legend_handles: # type: ignore
        handle._sizes = [30]
    plt.show()
    # analyze_iter_double_graph(data, iter_variable_name, iter_range)

    return output
