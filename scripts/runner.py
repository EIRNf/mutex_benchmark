from .constants import *
from .logger import logger

import subprocess

# TODO: save data files instead of deleting them every time (in their own folder)

def get_data_file_name(mutex_name, i, threads, rusage=False):
    if not(rusage):
        return f"{Constants.data_folder}/{mutex_name}-{threads}-{i}.csv"
    else:
        return f"{Constants.data_folder}/{mutex_name}-{threads}-{i}-rusage.csv"

def get_command(mutex_name, threads, *, csv=True, thread_level=False, rusage=False):
    args = [Constants.Defaults.EXECUTABLE, mutex_name, 
        str(threads), str(Constants.bench_n_seconds), str(Constants.noncritical_delay)]
    if csv:
        args.append("--csv")
    if thread_level:
        args.append("--thread-level")
    if rusage:
        args.append("--rusage")
    return args

# Should this be removed?
def run_experiment_multithreaded():
    # Run experiment
    # print("Running programs...")
    for mutex_name in Constants.mutex_names:
        # Create program threads
        threads = []
        for i in range(Constants.n_program_iterations):
            data_file_name = get_data_file_name(mutex_name, i, Constants.bench_n_threads)
            subprocess.run(["rm", "-f", data_file_name])
            command = get_command(mutex_name, Constants.bench_n_threads, csv=True, thread_level=True)
            thread = subprocess.Popen(command, stdout=subprocess.PIPE)
            threads.append([data_file_name, thread])
        # Collect data from threads
        for data_file_name, thread in threads:
            # print(f"Waiting on {data_file_name}")
            thread.wait()
            csv_data = thread.stdout.read()
            with open(data_file_name, "wb") as data_file:
                data_file.write(csv_data)

def run_experiment_single_threaded():
    # Run experiment
    for i in range(Constants.n_program_iterations):
        for mutex_name in Constants.mutex_names:
        # Create program threads
            threads = []
            logger.info(f"{mutex_name=} | {i=}")
            data_file_name = get_data_file_name(mutex_name, i, Constants.bench_n_threads)
            subprocess.run(["rm", "-f", data_file_name])
            command = get_command(mutex_name, Constants.bench_n_threads, csv=True, thread_level=True)
            thread = subprocess.run(command, stdout=subprocess.PIPE)
            csv_data = thread.stdout
            with open(data_file_name, "wb") as data_file:
                data_file.write(csv_data)

def run_experiment_lock_level_single_threaded():
    for i in range(Constants.n_program_iterations):
        for mutex_name in Constants.mutex_names:
            logger.info(f"{mutex_name=} | {i=}")
            data_file_name = get_data_file_name(mutex_name, i, Constants.bench_n_threads)
            subprocess.run(["rm", "-f", data_file_name])
            command = get_command(mutex_name, Constants.bench_n_threads, csv=True, thread_level=False)
            thread = subprocess.run(command, stdout=subprocess.PIPE)
            csv_data = thread.stdout
            with open(data_file_name, "wb") as data_file:
                data_file.write(csv_data)


def run_experiment_iter_v_threads_single_threaded(rusageGet=False):
    for threads in range(Constants.threads_start, Constants.threads_end, Constants.threads_step):
        for i in range(Constants.n_program_iterations):
            for mutex_name in Constants.mutex_names:
                logger.info(f"{mutex_name=} | {threads=} | {i=}")
                data_file_name = get_data_file_name(mutex_name, i, threads, rusage=rusageGet)
                subprocess.run(["rm", "-f", data_file_name])
                command = get_command(mutex_name, threads, csv=True, thread_level=True, rusage=rusageGet)
                thread=subprocess.run(command, stdout=subprocess.PIPE)
                csv_data = thread.stdout
                with open(data_file_name, "wb") as data_file:
                    data_file.write(csv_data)




# TODO: run_experiment_lock_level_multithreaded