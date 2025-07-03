import argparse
import logging

from .constants import Constants
from .logger import logger

def init_args():
    # TODO help=? for these arguments
    parser = argparse.ArgumentParser(
        prog='MutexTest',
        description='',
        epilog=''
    )
    parser.add_argument('threads', type=int)
    parser.add_argument('seconds', type=int)
    parser.add_argument('program_iterations', type=int)
    # parser.add_argument('threads_start', type=int, default=0, nargs='?')
    # parser.add_argument('threads_end',   type=int, default=0, nargs='?')
    # parser.add_argument('threads_step',  type=int, default=0, nargs='?')

    experiment_type = parser.add_mutually_exclusive_group()
    experiment_type.add_argument('--thread-level', action='store_true')
    experiment_type.add_argument('--lock-level', action='store_true')
    experiment_type.add_argument('--iter-v-threads', type=int, nargs=3)

    parser.add_argument('-l', '--log', type=str, default=Constants.Defaults.LOG)
    parser.add_argument('--data-folder', nargs='?', type=str, default=Constants.Defaults.DATA_FOLDER)
    parser.add_argument('--log-folder', nargs='?', type=str, default=Constants.Defaults.LOGS_FOLDER)
    
    mutex_names = parser.add_mutually_exclusive_group(required=True)
    mutex_names.add_argument('-i', '--include', nargs='+')
    mutex_names.add_argument('-x', '--exclude', nargs='+')
    mutex_names.add_argument('-a', '--all', action='store_true')
    
    parser.add_argument('--scatter', action='store_true', default=False)
    parser.add_argument('-m', '--multithreaded', action='store_true')
    parser.add_argument('-p', '--max-n-points', type=int, default=Constants.Defaults.MAX_N_POINTS, nargs='?')
    parser.add_argument('-n', '--noncritical-delay', type=int, default=1, nargs='?')

    log = parser.add_mutually_exclusive_group()
    log.add_argument('-d', '--debug', action='store_const', dest='log', const='DEBUG', default='DEBUG')
    log.add_argument('--info', action='store_const', dest='log', const='INFO',)
    log.add_argument('--warning', action='store_const', dest='log', const='WARNING',)
    log.add_argument('--error', action='store_const', dest='log', const='ERROR',)
    log.add_argument('--critical', action='store_const', dest='log', const='CRITICAL',)

    args = parser.parse_args()

    if args.all:
        Constants.mutex_names = Constants.Defaults.MUTEX_NAMES
    elif args.include:
        Constants.mutex_names = args.include
    elif args.exclude:
        Constants.mutex_names = [x for x in Constants.Defaults.MUTEX_NAMES if x not in args.exclude]
    
    Constants.bench_n_threads = args.threads
    Constants.bench_n_seconds = args.seconds
    Constants.n_program_iterations = args.program_iterations
    # Constants.threads_start = args.threads_start
    # Constants.threads_end = args.threads_end
    # Constants.threads_step = args.threads_step
    if args.iter_v_threads is not None:
        Constants.threads_start, Constants.threads_end, Constants.threads_step = args.iter_v_threads
    Constants.data_folder = args.data_folder
    logger.debug(Constants.data_folder)
    Constants.logs_folder = args.log_folder
    Constants.executable = Constants.Defaults.EXECUTABLE
    Constants.multithreaded = args.multithreaded
    Constants.thread_level = args.thread_level
    Constants.iter_v_threads = args.iter_v_threads
    Constants.scatter = args.scatter
    Constants.max_n_points = args.max_n_points
    Constants.noncritical_delay = args.noncritical_delay

    if args.log == "DEBUG":
        Constants.log = logging.DEBUG
    elif args.log == "INFO":
        Constants.log = logging.INFO
    elif args.log == "WARNING":
        Constants.log = logging.WARNING
    elif args.log == "ERROR":
        Constants.log = logging.ERROR
    elif args.log == "CRITICAL":
        Constants.log = logging.CRITICAL
    else:
        Constants.log = Constants.Defaults.LOG

    return args