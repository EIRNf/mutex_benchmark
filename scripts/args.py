# scripts/args.py

import argparse
import logging

from .constants import Constants
from .logger    import logger


def _ordered_dedupe(values):
    return list(dict.fromkeys(values))


def _resolve_mutex_names(args, parser):
    if args.all:
        mutex_names = list(Constants.Defaults.MUTEX_NAMES)
    elif args.set:
        mutex_names = []
        for set_name in args.set:
            mutex_set = Constants.Defaults.MUTEX_SETS.get(set_name)
            if mutex_set is None:
                available_sets = ", ".join(sorted(Constants.Defaults.MUTEX_SETS.keys()))
                parser.error(f"Unknown mutex set '{set_name}'. Available sets: {available_sets}")
            mutex_names.extend(mutex_set)
        mutex_names = _ordered_dedupe(mutex_names)
    elif args.include:
        mutex_names = list(args.include)
    else:
        mutex_names = [
            n for n in Constants.Defaults.MUTEX_NAMES
            if n not in args.exclude
        ]

    if args.exclude:
        for excluded_mutex_name in args.exclude:
            if excluded_mutex_name in mutex_names:
                mutex_names.remove(excluded_mutex_name)

    if args.include:
        for included_mutex_name in args.include:
            if included_mutex_name not in mutex_names:
                mutex_names.append(included_mutex_name)

    return mutex_names


def _apply_iter_config(args):
    if args.iter_threads is not None:
        Constants.iter_variable_name = "threads"
        Constants.iter_range = args.iter_threads
        Constants.iter = True
    elif args.iter_critical_delay is not None:
        Constants.iter_variable_name = "critical_delay"
        Constants.iter_range = args.iter_critical_delay
        Constants.iter = True
    elif args.iter_noncritical_delay is not None:
        Constants.iter_variable_name = "noncritical_delay"
        Constants.iter_range = args.iter_noncritical_delay
        Constants.iter = True
    else:
        Constants.iter = False

    if Constants.iter:
        Constants.iter_range[1] += 1  # End inclusive range


def _validate_grouped_constraints(args, parser):
    if args.bench == 'grouped':
        if args.groups is None or args.groups <= 0:
            parser.error("--groups must be provided and > 0 when --bench grouped")
        if not Constants.iter:
            parser.error("--bench grouped currently requires an --iter-* sweep")


def _resolve_executable_for_bench(bench_name):
    executable = Constants.Defaults.BENCH_EXECUTABLES.get(bench_name)
    if executable is None:
        raise NotImplementedError(f"Unknown executable: {bench_name}")
    return executable


def init_args():
    parser = argparse.ArgumentParser(
        prog='MutexTest',
        description='Run contention benchmarks on various mutex algorithms',
    )
    parser.add_argument('threads', type=int,
                        help="number of threads in contention")
    parser.add_argument('seconds', type=float,
                        help="run duration in seconds")
    parser.add_argument('program_iterations', type=int,
                        help="number of times to run c++ subscript (more if changing # threads or other parameter)")

    experiment_type = parser.add_mutually_exclusive_group()
    experiment_type.add_argument('--thread-level', action='store_true',
                     help='measure per-thread throughput')
    experiment_type.add_argument('--lock-level', action='store_true',
                     help='measure per-lock/unlock latency')
    experiment_type.add_argument('--iter-threads', type=int, nargs=3,
                     help='sweep iterations over a range of thread counts')
    experiment_type.add_argument('--iter-noncritical-delay', type=int, nargs=3)
    experiment_type.add_argument('--iter-critical-delay', type=int, nargs=3)
    
    
    parser.add_argument('-r', '--rusage', action='store_true', help = 'record CPU usage instead of time/# iterations')

    parser.add_argument('-l','--log', type=str, default='INFO',
                        help='console log level (DEBUG, INFO, WARNING, ERROR, CRITICAL)')
    parser.add_argument('--data-folder', type=str,
                        default=Constants.data_folder,
                        help='where to write CSV output')
    parser.add_argument('--log-folder',  type=str,
                        default=Constants.logs_folder,
                        help='where to write debug logs')

    # mnames = parser.add_mutually_exclusive_group(required=True)
    parser.add_argument('-i','--include', nargs='+',
                        help='only these mutex names')
    parser.add_argument('-x','--exclude', nargs='+',
                        help='all except these names')
    parser.add_argument('-a','--all', action='store_true',
                        help='run all default mutexes')
    parser.add_argument('-s', '--set', nargs='+',
                        help='run specific mutex sets')

    parser.add_argument('--scatter', action='store_true',
                        help='scatter CDF plots instead of lines')
    parser.add_argument('-m','--multithreaded', action='store_true',
                        help='spawn bench processes in parallel')
    parser.add_argument('-p','--max-n-points', type=int,
                        default=Constants.max_n_points,
                        help='max points to sample on the CDF')

    parser.add_argument('-n','--noncritical-delay', type=int, default=-1, nargs='?',
                        help='max iterations to busy sleep outside critical section')
    parser.add_argument('-c','--critical-delay', type=int, default=-1, nargs='?',
                        help='max iterations to busy sleep in critical section')

    parser.add_argument('--low-contention', action='store_true',
                        help='stagger thread startup to reduce initial contention')
    parser.add_argument('--stagger-ms',     type=int, default=0, nargs='?',
                        help='ms between each thread startup in low‐contention mode')
    parser.add_argument('--skip-experiment', action='store_true', default=False,
                        help="use previous data files instead of rerunning experiment (only works if exact same experiment was just run)")

    parser.add_argument('--scxl', action='store_true',
                        help='compile with emucxl allocation for cxl machine/software')
    parser.add_argument('--hcxl', action='store_true',
                        help='compile with emucxl allocation for cxl machine/hardware')

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

    parser.add_argument('--groups', type=int)
    parser.add_argument('--averages', action="store_true")
    parser.add_argument('--stdev', type=float, default=Constants.Defaults.STANDARD_DEVIATION_SCALE)

    parser.add_argument('--bench', type=str, default='max')
    parser.add_argument('--skip-plotting', action='store_true')
    parser.add_argument('--show', action='store_true',
                        help='display each plot interactively (blocks on plt.show()); '
                             'by default plots are only saved to the figs folder')
    parser.add_argument('--variability', action='store_true', default=False,
                     help='additionally plot coefficient of variation (σ/μ) vs iter variable')
    parser.add_argument('--speedup', nargs='?', const='exp_spin', default=None, metavar='REF_MUTEX',
                     help='print speedup table relative to REF_MUTEX (default: exp_spin)')

    plot_layout = parser.add_mutually_exclusive_group()
    plot_layout.add_argument('--faceted', action='store_true', default=False,
                     help='force faceted subplot layout grouped by mutex family')
    plot_layout.add_argument('--combined', action='store_true', default=False,
                     help='force single combined plot even with many series')

    # parser.add_argument('--numactl',  action='store_true',
    #                     help='use numactl to bind memory')

    args = parser.parse_args()

    Constants.mutex_names = _resolve_mutex_names(args, parser)

    Constants.bench_n_threads      = args.threads
    Constants.bench_n_seconds      = args.seconds
    Constants.n_program_iterations = args.program_iterations
    Constants.averages = args.averages
    # Constants.threads_start = args.threads_start
    # Constants.threads_end = args.threads_endf
    # Constants.threads_step = args.threads_step
    Constants.rusage = args.rusage

    _apply_iter_config(args)
    _validate_grouped_constraints(args, parser)

    Constants.data_folder = args.data_folder
    logger.debug(Constants.data_folder)
    Constants.logs_folder = args.log_folder
    Constants.executable = Constants.Defaults.EXECUTABLE
    Constants.multithreaded = args.multithreaded
    Constants.thread_level = args.thread_level or Constants.iter
    Constants.scatter = args.scatter
    Constants.bench = args.bench
    Constants.groups = args.groups
    Constants.stdev_scale = args.stdev
    # Constants.numactl = args.numactl

    Constants.executable = _resolve_executable_for_bench(args.bench)
    
    Constants.max_n_points = args.max_n_points

    Constants.noncritical_delay = args.noncritical_delay
    Constants.critical_delay = args.critical_delay
    Constants.skip_experiment = args.skip_experiment

    Constants.low_contention = args.low_contention
    Constants.stagger_ms     = args.stagger_ms
    Constants.skip_plotting = args.skip_plotting
    Constants.show = args.show
    Constants.variability = args.variability
    Constants.speedup = args.speedup is not None
    Constants.speedup_ref = args.speedup
    if args.faceted:
        Constants.faceted = True
    elif args.combined:
        Constants.faceted = False
    else:
        Constants.faceted = None  # auto-decide based on series count
    Constants.software_cxl = args.scxl
    Constants.hardware_cxl = args.hcxl

    # if Constants.cxl:
    #     for mutex_name in Constants.mutex_names:
    #         assert mutex_name in Constants.Defaults.CXL_MUTEXES, "expected only mutexes that work on cxl"

    level = getattr(logging, args.log.upper(), Constants.Defaults.LOG)
    Constants.log = level
    logger.setLevel(level)

    return args
