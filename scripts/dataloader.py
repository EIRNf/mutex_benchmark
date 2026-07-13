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
            if not _file_ready(data_file_name, mutex_name, context=f"rep={i}"):
                continue
            for chunk_i, df in enumerate(pd.read_csv(data_file_name, names=["Thread ID", "Iteration #", "Time Spent"], chunksize=CHUNKSIZE)):
                current_average = df["Time Spent"].mean()
                current_count = df["Time Spent"].size
                logger.debug(f"\tMutex {mutex_name:<20}: loaded chunk #{chunk_i:0>3}... {current_average=:.12f} | {current_count=:>13}")
                total_average = (total_average * total_count + current_average * current_count) / (total_count + current_count)
                total_count += current_count
        logger.info(f"Mutex {mutex_name:>20}: mean time spent: {total_average:.12f} | datapoint count: {total_count:>13}")
    logger.debug("print_averages_lock_level done.")


def _file_ready(path: str, mutex_name: str, *, context: str = "") -> bool:
    """Return True if *path* exists and is non-empty; log a warning otherwise."""
    import os
    ctx = f" ({context})" if context else ""
    if not os.path.exists(path):
        logger.warning(f"Missing data file for {mutex_name}{ctx} — was this point skipped due to a hang? ({path})")
        return False
    if os.path.getsize(path) == 0:
        logger.warning(f"Empty data file for {mutex_name}{ctx} — skipping ({path})")
        return False
    return True


def load_data_lock_level():
    """
    Loads saved data from csv files in DATA_FOLDER.
    Generates tuples of the form (mutex_name, dataframe).
    Silently skips any mutex whose CSV files are entirely missing (hung runs).
    """
    logger.debug("load_data_lock_level running...")
    for mutex_name in Constants.mutex_names:
        dataframes = []
        for i in range(Constants.n_program_iterations):
            data_file_name = get_data_file_name(mutex_name, i)
            if not _file_ready(data_file_name, mutex_name, context=f"rep={i}"):
                continue
            dataframe = pd.read_csv(data_file_name, names=["Thread ID", "Iteration #", "Time Spent"])
            dataframes.append(dataframe)
        if not dataframes:
            logger.warning(f"No data at all for {mutex_name} — skipping from plot")
            continue
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
            # Use only the sweep variable in the file-name lookup (rusage is
            # encoded in the capture segment, not as a separate kwarg).
            iter_kwargs = {Constants.iter_variable_name: iter_variable_value}
            dataframes = []
            for i in range(Constants.n_program_iterations):
                data_file_name = get_data_file_name(mutex_name, i, **iter_kwargs)
                if not _file_ready(data_file_name, mutex_name, context=f"{Constants.iter_variable_name}={iter_variable_value}, rep={i}"):
                    continue
                dataframe = pd.read_csv(data_file_name, names=get_column_names(Constants.rusage))
                dataframe["run"] = i
                dataframes.append(dataframe)
            if not dataframes:
                # All reps at this point were skipped — no rows for this value.
                continue
            df_point = pd.concat(dataframes)
            df_point[Constants.iter_variable_name] = iter_variable_value
            all_dataframes.append(df_point)
        if not all_dataframes:
            logger.warning(f"No data at all for {mutex_name} — skipping from plot")
            continue
        yield mutex_name, pd.concat(all_dataframes)

