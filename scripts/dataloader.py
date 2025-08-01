from .constants import *
from .runner import get_data_file_name
from .logger import logger

import pandas as pd # pyright: ignore[reportMissingModuleSource]

# Load data from CSV into dictionary of Pandas dataframes

def debug_print_averages_lock_level():
    CHUNKSIZE = 1_000_000
    logger.debug("print_averages_lock_level running...")
    for mutex_name in Constants.mutex_names:
        total_average = 0
        total_count = 0
        for i in range(Constants.n_program_iterations):
            data_file_name = get_data_file_name(mutex_name, i)
            for i, df in enumerate(pd.read_csv(data_file_name, names=["Thread ID", "Iteration #", "Time Spent"], chunksize=CHUNKSIZE)):
                current_average = df["Time Spent"].mean()
                current_count = df["Time Spent"].size
                logger.debug(f"\tMutex {mutex_name:<20}: loaded chunk #{i:0>3}... {current_average=:.12f} | {current_count=:>13}")
                total_average = (total_average * total_count + current_average * current_count) / (total_count + current_count)
                total_count += current_count
        logger.info(f"Mutex {mutex_name:>20}: mean time spent: {total_average:.12f} | datapoint count: {total_count:>13}")
    logger.debug("print_averages_lock_level done.")


def load_data_lock_level():
    """
    Loads saved data from csv files in DATA_FOLDER.
    Generates tuples of the form (mutex_name, dataframe).
    """
    logger.debug("load_data_lock_level running...")
    for mutex_name in Constants.mutex_names:
        dataframes = []
        for i in range(Constants.n_program_iterations):
            data_file_name = get_data_file_name(mutex_name, i)
            dataframe = pd.read_csv(data_file_name, names=["Thread ID", "Iteration #", "Time Spent"])
            dataframes.append(dataframe)
        logger.debug(f"load_data_lock_level: Loaded data for {mutex_name}, yielding...")
        yield (mutex_name, pd.concat(dataframes))
    logger.debug("load_data_lock_level done.")

def get_column_names(rusage):
    if rusage:
        return ["utime", "stime", "maxrss", "ru_minflt", "ru_majflt"]
    else:
        return ["Thread ID", "Seconds", "# Iterations"]

def load_data_iter():
    for mutex_name in Constants.mutex_names:
        all_dataframes = []
        for iter_variable_value in range(*Constants.iter_range):
            extra_command_args = {Constants.iter_variable_name:iter_variable_value, "rusage":Constants.rusage}
            dataframes=[]
            for i in range(Constants.n_program_iterations):
                data_file_name = get_data_file_name(mutex_name, i, **extra_command_args)
                dataframe = pd.read_csv(data_file_name, names=get_column_names(Constants.rusage))
                dataframes.append(dataframe)
            dataframes=pd.concat(dataframes)
            dataframes[Constants.iter_variable_name] = iter_variable_value
            all_dataframes.append(dataframes)
        yield mutex_name, pd.concat(all_dataframes)
