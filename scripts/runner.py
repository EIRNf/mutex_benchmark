from .constants import *
from .logger import logger

import subprocess

# TODO: save data files instead of deleting them every time (in their own folder)

def get_data_file_name(mutex_name, i, **kwargs):
    name_root = f"{Constants.data_folder}/{mutex_name}-{i}"
    for name, value in kwargs.items():
        name_root += f"-{name}={value}"
    name = name_root + ".csv"
    return name

def get_command(mutex_name, *, threads=None, csv=True, thread_level=False, critical_delay=None, noncritical_delay=None):
    if threads is None:
        threads = Constants.bench_n_threads
    if critical_delay is None:
        critical_delay = Constants.critical_delay
    if noncritical_delay is None:
        noncritical_delay = Constants.noncritical_delay
    args = [
        Constants.Defaults.EXECUTABLE, 
        mutex_name, 
        str(threads), 
        str(Constants.bench_n_seconds), 
        str(critical_delay), 
        str(noncritical_delay),
    ]
    if csv:
        args.append("--csv")
    if thread_level:
        args.append("--thread-level")
    return args

# # Should this be removed?
# def run_experiment_multithreaded():
#     # Run experiment
#     # print("Running programs...")
#     for mutex_name in Constants.mutex_names:
#         # Create program threads
#         threads = []
#         for i in range(Constants.n_program_iterations):
#             data_file_name = get_data_file_name(mutex_name, i, Constants.bench_n_threads)
#             subprocess.run(["rm", "-f", data_file_name])
#             command = get_command(mutex_name, Constants.bench_n_threads, csv=True, thread_level=True)
#             thread = subprocess.Popen(command, stdout=subprocess.PIPE)
#             threads.append([data_file_name, thread])
#         # Collect data from threads
#         for data_file_name, thread in threads:
#             # print(f"Waiting on {data_file_name}")
#             thread.wait()
#             csv_data = thread.stdout.read()
#             with open(data_file_name, "wb") as data_file:
#                 data_file.write(csv_data)

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


def run_experiment_iter_v_threads_single_threaded():
    # TODO replace with generic "run_experiment_iter_single_threaded"
    for threads in range(*Constants.iter_threads):
        for i in range(Constants.n_program_iterations):
            for mutex_name in Constants.mutex_names:
                logger.info(f"{mutex_name=} | {threads=} | {i=}")
                data_file_name = get_data_file_name(mutex_name, i)
                subprocess.run(["rm", "-f", data_file_name])
                command = get_command(mutex_name, threads=threads, csv=True, thread_level=True)
                thread=subprocess.run(command, stdout=subprocess.PIPE)
                csv_data = thread.stdout
                with open(data_file_name, "wb") as data_file:
                    data_file.write(csv_data)


def run_experiment_iter_single_threaded(iter_variable_name, iter):
    for i in range(Constants.n_program_iterations):
        for iter_variable_value in range(*iter):
            extra_command_args = {iter_variable_name: iter_variable_value}
            for mutex_name in Constants.mutex_names:
                logger.info(f"{mutex_name=} | {i=} | {extra_command_args=}")
                data_file_name = get_data_file_name(mutex_name, i, **extra_command_args)
                subprocess.run(["rm", "-f", data_file_name])
                command = get_command(mutex_name, csv=True, thread_level=False, **extra_command_args)
                thread = subprocess.run(command, stdout=subprocess.PIPE)
                csv_data = thread.stdout
                with open(data_file_name, "wb") as data_file:
                    data_file.write(csv_data)



# TODO: run_experiment_lock_level_multithreaded