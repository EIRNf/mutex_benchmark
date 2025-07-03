from .constants import *
from .logger import logger

import subprocess

# TODO: save data files instead of deleting them every time (in their own folder)

def get_data_file_name(mutex_name, i):
    return f"{Constants.data_folder}/{mutex_name}-{i}.csv"

def get_command(mutex_name, *, csv=True, thread_level=False):
    args = [Constants.executable, mutex_name, 
        str(Constants.bench_n_threads), str(Constants.bench_n_seconds)]
    
    if Constants.bench=="grouped":
        args.append(str(Constants.groups))
    if csv:
        args.append("--csv")
    if thread_level:
        args.append("--thread-level")
    return args


# Should this be removed?
def run_experiment_multithreaded():
    # Run experiment
    # print("Running programs...")
    for mutex_name in Constants.mutex_names:
        # Create program threads
        threads = []
        for i in range(Constants.n_program_iterations):
            data_file_name = get_data_file_name(mutex_name, i)
            subprocess.run(["rm", "-f", data_file_name])
            command = get_command(mutex_name, csv=True, thread_level=True)
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
            data_file_name = get_data_file_name(mutex_name, i)
            subprocess.run(["rm", "-f", data_file_name])
            command = get_command(mutex_name, csv=True, thread_level=True)
            thread = subprocess.run(command, stdout=subprocess.PIPE)
            csv_data = thread.stdout
            with open(data_file_name, "wb") as data_file:
                data_file.write(csv_data)

def run_experiment_lock_level_single_threaded():
    for i in range(Constants.n_program_iterations):
        for mutex_name in Constants.mutex_names:
            logger.info(f"{mutex_name=} | {i=}")
            data_file_name = get_data_file_name(mutex_name, i)
            subprocess.run(["rm", "-f", data_file_name])
            command = get_command(mutex_name, csv=True, thread_level=False)
            thread = subprocess.run(command, stdout=subprocess.PIPE)
            csv_data = thread.stdout
            with open(data_file_name, "wb") as data_file:
                data_file.write(csv_data)

def run_grouped_experiment_thread_level_single_threaded():
    # Run experiment
    for i in range(Constants.n_program_iterations):
        for mutex_name in Constants.mutex_names:
        # Create program threads
            threads = []
            logger.info(f"{mutex_name=} | {i=}")
            data_file_name = get_data_file_name(mutex_name, i)
            subprocess.run(["rm", "-f", data_file_name])
            command = get_command(mutex_name, csv=True, thread_level=True)
            thread = subprocess.run(command, stdout=subprocess.PIPE)
            csv_data = thread.stdout
            with open(data_file_name, "wb") as data_file:
                data_file.write(csv_data)


def get_data_file_name_threads(mutex_name, i, threads):
    return f"{Constants.data_folder}/{mutex_name}-{threads}-{i}.csv"


def get_command_threads(mutex_name, threads, *, csv=True, thread_level=False):
    args = [Constants.executable, mutex_name, 
        str(threads), str(Constants.bench_n_seconds)]
    
    if Constants.bench=="grouped":
        args.append(str(Constants.groups))
    if csv:
        args.append("--csv")
    if thread_level:
        args.append("--thread-level")
    return args

def run_experiment_iter_v_threads_single_threaded():
    for threads in range(Constants.threads_start, Constants.threads_end, Constants.threads_step):
        for i in range(Constants.n_program_iterations):
            for mutex_name in Constants.mutex_names:
                logger.info(f"{mutex_name=} | {threads=} | {i=}")
                data_file_name = get_data_file_name_threads(mutex_name, i, threads)
                subprocess.run(["rm", "-f", data_file_name])
                command = get_command_threads(mutex_name, threads, csv=True)
                thread=subprocess.run(command, stdout=subprocess.PIPE)
                csv_data = thread.stdout
                with open(data_file_name, "wb") as data_file:
                    data_file.write(csv_data)

def run_grouped_experiment_iter_v_threads_single_threaded():
    for threads in range(Constants.threads_start, Constants.threads_end, Constants.threads_step):
        for i in range(Constants.n_program_iterations):
            for mutex_name in Constants.mutex_names:
                logger.info(f"{mutex_name=} | {threads=} | {i=}")
                data_file_name = get_data_file_name_threads(mutex_name, i, threads)
                subprocess.run(["rm", "-f", data_file_name])
                command = get_command_threads(mutex_name, threads, csv=True)
                print(command[0])
                thread=subprocess.run(command, stdout=subprocess.PIPE)
                csv_data = thread.stdout
                with open(data_file_name, "wb") as data_file:
                    data_file.write(csv_data)



# TODO: run_experiment_lock_level_multithreaded