from .constants import *

import pandas as pd

# Load data from CSV into dictionary of Pandas dataframes

def load_data():
    data = {}
    for mutex_name in Constants.mutex_names:
        dataframes = []
        for i in range(Constants.n_program_iterations):
            data_file_name = f"{Constants.data_folder}/{mutex_name}-{i}.csv"
            dataframe = pd.read_csv(data_file_name, names=["Thread #", "# Iterations", "Time Spent"])
            dataframes.append(dataframe)
        data[mutex_name] = pd.concat(dataframes)
    return data

def load_data_lock_level():
    data = {}
    for mutex_name in Constants.mutex_names:
        dataframes = []
        for i in range(Constants.n_program_iterations):
            data_file_name = f"{Constants.data_folder}/{mutex_name}-{i}.csv"
            dataframe = pd.read_csv(data_file_name, names=["Thread ID", "Iteration #", "Time Spent"])
            dataframes.append(dataframe)
        data[mutex_name] = pd.concat(dataframes)
    return data
