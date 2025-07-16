from .constants import *
from .runner import get_data_file_name

import pandas as pd

# Load data from CSV into dictionary of Pandas dataframes

def load_data():
    data = {}
    for mutex_name in Constants.mutex_names:
        dataframes = []
        for i in range(Constants.n_program_iterations):
            data_file_name = get_data_file_name(mutex_name, i)
            dataframe = pd.read_csv(data_file_name, names=["Thread #", "Seconds", "# Iterations"])
            dataframes.append(dataframe)
        data[mutex_name] = pd.concat(dataframes)
    return data

def load_data_lock_level():
    data = {}
    for mutex_name in Constants.mutex_names:
        dataframes = []
        for i in range(Constants.n_program_iterations):
            data_file_name = get_data_file_name(mutex_name, i)
            dataframe = pd.read_csv(data_file_name, names=["Thread ID", "Iteration #", "Time Spent"])
            dataframes.append(dataframe)
        data[mutex_name] = pd.concat(dataframes)
    return data

# def load_data_iter_v_threads():
#     if (Constants.iter_threads)
#     data = {}
#     for mutex_name in Constants.mutex_names:
#         data[mutex_name]=[]
#         for threads in range(*Constants.iter_threads):
#             dataframes=[]
#             for i in range(Constants.n_program_iterations):
#                 data_file_name = get_data_file_name(mutex_name, i, threads=threads)
#                 dataframe = pd.read_csv(data_file_name, names=["Thread ID", "Seconds", "# Iterations"])
#                 dataframes.append(dataframe)
#             dataframes=pd.concat(dataframes)
#             data[mutex_name].append(dataframes["# Iterations"])
#     return data

def load_data_iter(iter_variable_name, iter_range):
    data = {}
    for mutex_name in Constants.mutex_names:
        data[mutex_name]=[]
        for iter_variable_value in range(*iter_range):
            extra_command_args = {iter_variable_name:iter_variable_value}
            dataframes=[]
            for i in range(Constants.n_program_iterations):
                data_file_name = get_data_file_name(mutex_name, i, **extra_command_args)
                dataframe = pd.read_csv(data_file_name, names=["Thread ID", "Iteration #", "Time Spent"])
                dataframes.append(dataframe)
            dataframes=pd.concat(dataframes)
            dataframes[iter_variable_name] = iter_variable_value
            data[mutex_name].append(dataframes)
        data[mutex_name] = pd.concat(data[mutex_name])
    return data
