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

def run_experiment_thread_level():
    if Constants.multithreaded:
        run_experiment_multi_threaded()
    else:
        run_experiment_single_threaded()
    data = load_data()
    output = analyze(data)
    print(output)
    logger.info(output)

def run_experiment_lock_level():
    run_experiment_lock_level_single_threaded()
    data = load_data_lock_level()
    output = analyze_lock_level(data)
    print(output)
    logger.info(output)

def run_experiment_iter_v_threads():
    if not(Constants.rusage):
        run_experiment_iter_v_threads_single_threaded()
        data = load_data_iter_v_threads()
        output = analyze_iter_v_threads(data)
        print(output)
        logger.info(output)
    else: 
        # run_experiment_iter_v_threads_single_threaded(rusageGet=True)
        data = load_data_iter_v_threads(rusage=True)
        output = analyze_iter_v_threads_rusage(data)
        print(output)
        logger.info(output)


def main():
    init_args()
    init_logger()
    setup()
    build()
    if Constants.thread_level:
        run_experiment_thread_level()
    elif Constants.iter_v_threads:
        run_experiment_iter_v_threads()
    else:
        run_experiment_lock_level()

if __name__ == "__main__":
    main()