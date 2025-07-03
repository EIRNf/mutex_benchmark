from .constants import *
from .runner import get_data_file_name

import pandas as pd

# Load data from CSV into dictionary of Pandas dataframes

def load_data():
    data = {}
    for mutex_name in Constants.mutex_names:
        dataframes = []
        for i in range(Constants.n_program_iterations):
            data_file_name = get_data_file_name(mutex_name, i, Constants.bench_n_threads)
            dataframe = pd.read_csv(data_file_name, names=["Thread #", "Seconds", "# Iterations"])
            dataframes.append(dataframe)
        data[mutex_name] = pd.concat(dataframes)
    return data

def load_data_lock_level():
    data = {}
    for mutex_name in Constants.mutex_names:
        dataframes = []
        for i in range(Constants.n_program_iterations):
            data_file_name = get_data_file_name(mutex_name, i, Constants.bench_n_threads)
            dataframe = pd.read_csv(data_file_name, names=["Thread ID", "Iteration #", "Time Spent"])
            dataframes.append(dataframe)
        data[mutex_name] = pd.concat(dataframes)
    return data

def load_data_iter_v_threads():
    data = {}
    for mutex_name in Constants.mutex_names:
        data[mutex_name]=[]
        for threads in range(Constants.threads_start, Constants.threads_end, Constants.threads_step):
            dataframes=[]
            for i in range(Constants.n_program_iterations):
                data_file_name = f"{Constants.data_folder}/{mutex_name}-{threads}-{i}.csv"
                dataframe = pd.read_csv(data_file_name, names=["Thread ID", "Seconds", "# Iterations"])
                dataframes.append(dataframe)
            dataframes=pd.concat(dataframes)
            data[mutex_name].append(dataframes["# Iterations"])
    return data
