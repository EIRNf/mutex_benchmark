from .runner import *
from .dataloader import *
from .analyzer import *
from .builder import *
from .logger import init_logger, logger
from .args import init_args

import os

def setup():
    # Make sure the script is being run from the right location (in mutex_benchmark directory)
    absolute_path = os.path.abspath(__file__)
    parent_directory = os.path.dirname(absolute_path)
    os.chdir(parent_directory + "/..")

# def run_experiment_thread_level():
#     if Constants.multithreaded:
#         raise NotImplementedError()
#     else:
#         run_experiment_single_threaded()
#     data = load_data()
#     output = analyze(data)
#     print(output)
#     logger.info(output)

def run_experiment_lock_level():
    if not Constants.skip_experiment:
        run_experiment_lock_level_single_threaded()
    data = load_data_lock_level()
    output = analyze_lock_level(data)
    print(output)
    logger.info(output)

def run_experiment_iter_v_threads():
    # run_experiment_iter_v_threads_single_threaded()
    # data = load_data_iter_v_threads()
    # output = analyze_iter_v_threads(data)
    # print(output)
    # logger.info(output)
    run_experiment_iter("threads", Constants.iter_threads)

def run_experiment_iter(iter_variable_name, iter_range, *, thread_level=False):
    iter_range[1] += 1 # To make the range inclusive, we need to add one to the end value. ([start, end, step])
    if not Constants.skip_experiment:
        # Normal experiment
        run_experiment_iter_single_threaded(iter_variable_name, iter_range, thread_level=thread_level)
    data = load_data_iter(iter_variable_name, iter_range)
    output = analyze_iter(data, iter_variable_name, iter_range)
    print(output)
    logger.info(output)


# def run_grouped_experiment_thread_level():
#     run_grouped_experiment_thread_level_single_threaded()
#     data = load_data()
#     output = analyze(data)
#     print(output)
#     logger.info(output)

# def run_grouped_experiment_iter_v_threads():

def main():
    setup()
    build()
    init_args()
    init_logger()
    if Constants.iter_threads is not None:
        run_experiment_iter_v_threads()
    elif Constants.iter_noncritical_delay is not None:
        run_experiment_iter("noncritical_delay", iter_range=Constants.iter_noncritical_delay)
    elif Constants.iter_critical_delay is not None:
        run_experiment_iter("critical_delay", iter_range=Constants.iter_critical_delay)

    if Constants.bench == 'max':
        run_experiment_lock_level()
    elif Constants.bench == 'grouped':
        run_experiment_iter("threads", iter_range=Constants.iter_threads, thread_level=Constants.thread_level)
    elif Constants.bench == 'min':
        run_experiment_lock_level()
    else:
        raise NotImplementedError(f"Benchmark {Constants.bench} not recognized.")


if __name__ == "__main__":
    main()