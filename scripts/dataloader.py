from .runner import get_data_file_name
from .logger import logger

import pandas as pd # pyright: ignore[reportMissingModuleSource]

# Load data from CSV into dictionary of Pandas dataframes.
#
# max_contention_bench CSVs start with a "# key=value ..." metadata line
# (read_metadata). Its thread-level files then have a header row; older
# files have neither and hold only thread id, seconds and iterations.

LOCK_LEVEL_COLUMNS = ["Thread ID", "Iteration #", "Time Spent"]
LEGACY_THREAD_COLUMNS = ["Thread ID", "Seconds", "# Iterations"]
THREAD_COLUMNS = {
    "thread_id": "Thread ID",
    "seconds": "Seconds",
    "iterations": "# Iterations",
    "reacquires": "Reacquires",
    "cpu_pinned": "CPU pinned",
    "cpu_start": "CPU start",
    "cpu_end": "CPU end",
    "node_end": "Node",
}


def read_metadata(path: str) -> dict:
    """The key=value pairs of a data file's leading "# ..." line ({} if none)."""
    try:
        with open(path, encoding="utf-8") as handle:
            first = handle.readline()
    except OSError:
        return {}
    if not first.startswith("#"):
        return {}
    metadata = {}
    for token in first[1:].split():
        key, sep, value = token.partition("=")
        if sep:
            metadata[key] = value
    return metadata


def _first_data_line(path: str) -> str:
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            if not line.startswith("#"):
                return line
    return ""


def read_thread_level_csv(path: str) -> pd.DataFrame:
    """One row per thread, in either the current or the legacy layout."""
    if _first_data_line(path).startswith("thread_id"):
        return pd.read_csv(path, comment="#").rename(columns=THREAD_COLUMNS)
    return pd.read_csv(path, names=LEGACY_THREAD_COLUMNS, comment="#")

def debug_print_averages_lock_level(run):
    CHUNKSIZE = 1_000_000
    logger.debug("print_averages_lock_level running...")
    for mutex_name in run.mutex_names:
        total_average = 0
        total_count = 0
        for i in range(run.n_program_iterations):
            data_file_name = get_data_file_name(run, mutex_name, i)
            if not _file_ready(data_file_name, mutex_name, context=f"rep={i}"):
                continue
            for chunk_i, df in enumerate(pd.read_csv(data_file_name, names=LOCK_LEVEL_COLUMNS, comment="#", chunksize=CHUNKSIZE)):
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


def load_data_lock_level(run):
    """
    Loads saved data from the run's CSV files (run.data_folder).
    Generates tuples of the form (mutex_name, dataframe).
    Silently skips any mutex whose CSV files are entirely missing (hung runs).
    """
    logger.debug("load_data_lock_level running...")
    for mutex_name in run.mutex_names:
        dataframes = []
        for i in range(run.n_program_iterations):
            data_file_name = get_data_file_name(run, mutex_name, i)
            if not _file_ready(data_file_name, mutex_name, context=f"rep={i}"):
                continue
            dataframe = pd.read_csv(data_file_name, names=LOCK_LEVEL_COLUMNS, comment="#")
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
        return list(LEGACY_THREAD_COLUMNS)


def read_point(path: str, rusage: bool) -> pd.DataFrame:
    if rusage:
        return pd.read_csv(path, names=get_column_names(True), comment="#")
    return read_thread_level_csv(path)

def load_data_iter(run):
    for mutex_name in run.mutex_names:
        all_dataframes = []
        for iter_variable_value in range(*run.iter_range):
            # Use only the sweep variable in the file-name lookup (rusage is
            # encoded in the capture segment, not as a separate kwarg).
            iter_kwargs = {run.iter_variable_name: iter_variable_value}
            dataframes = []
            for i in range(run.n_program_iterations):
                data_file_name = get_data_file_name(run, mutex_name, i, **iter_kwargs)
                if not _file_ready(data_file_name, mutex_name, context=f"{run.iter_variable_name}={iter_variable_value}, rep={i}"):
                    continue
                dataframe = read_point(data_file_name, run.rusage)
                dataframe["run"] = i
                dataframes.append(dataframe)
            if not dataframes:
                # All reps at this point were skipped — no rows for this value.
                continue
            df_point = pd.concat(dataframes)
            df_point[run.iter_variable_name] = iter_variable_value
            all_dataframes.append(df_point)
        if not all_dataframes:
            logger.warning(f"No data at all for {mutex_name} — skipping from plot")
            continue
        yield mutex_name, pd.concat(all_dataframes)

