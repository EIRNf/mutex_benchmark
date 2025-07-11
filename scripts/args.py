# scripts/args.py

import argparse
import logging

from .constants import Constants
from .logger    import logger

def init_args():
    parser = argparse.ArgumentParser(
        prog='MutexTest',
        description='Run contention benchmarks on various mutex algorithms',
    )

    parser.add_argument('threads',            type=int,
                        help='number of threads to spawn')
    parser.add_argument('seconds',            type=int,
                        help='benchmark runtime in seconds')
    parser.add_argument('program_iterations', type=int,
                        help='how many CSV files / iterations to run')

    exp = parser.add_mutually_exclusive_group()
    exp.add_argument('--thread-level', action='store_true',
                     help='measure per-thread throughput')
    exp.add_argument('--lock-level',   action='store_true',
                     help='measure per-lock/unlock latency')
    exp.add_argument('--iter-v-threads', nargs=3, type=int, metavar=('START','END','STEP'),
                     help='sweep iterations over a range of thread counts')

    parser.add_argument('-l','--log', type=str, default='INFO',
                        help='console log level (DEBUG, INFO, WARNING, ERROR, CRITICAL)')
    parser.add_argument('--data-folder', type=str,
                        default=Constants.data_folder,
                        help='where to write CSV output')
    parser.add_argument('--log-folder',  type=str,
                        default=Constants.logs_folder,
                        help='where to write debug logs')

    mnames = parser.add_mutually_exclusive_group(required=True)
    mnames.add_argument('-i','--include', nargs='+',
                        help='only these mutex names')
    mnames.add_argument('-x','--exclude', nargs='+',
                        help='all except these names')
    mnames.add_argument('-a','--all', action='store_true',
                        help='run all default mutexes')

    parser.add_argument('--scatter', action='store_true',
                        help='scatter CDF plots instead of lines')
    parser.add_argument('-m','--multithreaded', action='store_true',
                        help='spawn bench processes in parallel')
    parser.add_argument('-p','--max-n-points', type=int,
                        default=Constants.max_n_points,
                        help='max points to sample on the CDF')

    parser.add_argument('-n','--noncritical-delay', type=int, default=1, nargs='?',
                        help='max nanoseconds to sleep outside critical section')

    parser.add_argument('--low-contention', action='store_true',
                        help='stagger thread startup to reduce initial contention')
    parser.add_argument('--stagger-ms',     type=int, default=0, nargs='?',
                        help='ms between each thread startup in low‐contention mode')

    logg = parser.add_mutually_exclusive_group()
    logg.add_argument('-d','--debug',    action='store_const', dest='log', const='DEBUG',
                      help='set log level to DEBUG')
    logg.add_argument('--info',          action='store_const', dest='log', const='INFO',
                      help='set log level to INFO')
    logg.add_argument('--warning',       action='store_const', dest='log', const='WARNING',
                      help='set log level to WARNING')
    logg.add_argument('--error',         action='store_const', dest='log', const='ERROR',
                      help='set log level to ERROR')
    logg.add_argument('--critical',      action='store_const', dest='log', const='CRITICAL',
                      help='set log level to CRITICAL')

    args = parser.parse_args()

    if args.all:
        Constants.mutex_names = Constants.Defaults.MUTEX_NAMES
    elif args.include:
        Constants.mutex_names = args.include
    else:  
        Constants.mutex_names = [
            n for n in Constants.Defaults.MUTEX_NAMES
            if n not in args.exclude
        ]

    Constants.bench_n_threads      = args.threads
    Constants.bench_n_seconds      = args.seconds
    Constants.n_program_iterations = args.program_iterations

    Constants.iter_v_threads = args.iter_v_threads
    if args.iter_v_threads:
        Constants.threads_start, Constants.threads_end, Constants.threads_step = args.iter_v_threads

    Constants.data_folder       = args.data_folder
    Constants.logs_folder       = args.log_folder
    Constants.multithreaded     = args.multithreaded
    Constants.thread_level      = args.thread_level
    Constants.scatter           = args.scatter
    Constants.max_n_points      = args.max_n_points
    Constants.noncritical_delay = args.noncritical_delay

    Constants.low_contention = args.low_contention
    Constants.stagger_ms     = args.stagger_ms

    level = getattr(logging, args.log.upper(), Constants.Defaults.LOG)
    Constants.log = level
    logger.setLevel(level)

    return args
