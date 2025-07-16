# scripts/constants.py

import logging

class Constants:
    class Defaults:
        MUTEX_NAMES = [
            "burns_lamport",
            # "dijkstra",
            # "bakery",
            "spin",
            "exp_spin",
            "nsync",
            # "pthread",
            "mcs",
            # "knuth",
            # "peterson",
            # "clh",
            "hopscotch",
            "ticket", 
            # "halfnode", 
            # "lamport", 
            # "boulangerie",
            "tree_cas_elevator",
            "linear_cas_elevator",
            "tree_bl_elevator",
            "linear_bl_elevator",
            "futex",
            "szymanski",
        ]
        EXECUTABLE_NAME = "max_contention_bench"
        BENCH_N_THREADS = 10
        BENCH_N_SECONDS = 1
        N_PROGRAM_ITERATIONS = 10
        DATA_FOLDER          = "./data/generated"
        LOGS_FOLDER          = "./data/logs"
        EXECUTABLE           = f"./build/apps/{EXECUTABLE_NAME}/{EXECUTABLE_NAME}"
        MULTITHREADED        = False
        THREAD_LEVEL         = True
        SCATTER              = False
        LOG                  = logging.INFO
        SKIP                 = 1
        MAX_N_POINTS         = 1000
        LOG_SCALE            = True

        LOW_CONTENTION = False
        STAGGER_MS     = 0
        BENCH = 'max'

    mutex_names          = Defaults.MUTEX_NAMES
    bench_n_threads: int = Defaults.BENCH_N_THREADS
    bench_n_seconds: int = Defaults.BENCH_N_SECONDS
    n_program_iterations = Defaults.N_PROGRAM_ITERATIONS
    data_folder          = Defaults.DATA_FOLDER
    logs_folder          = Defaults.LOGS_FOLDER
    log_scale            = Defaults.LOG_SCALE
    executable           = Defaults.EXECUTABLE
    multithreaded        = Defaults.MULTITHREADED
    thread_level         = Defaults.THREAD_LEVEL
    scatter              = Defaults.SCATTER
    max_n_points         = Defaults.MAX_N_POINTS
    log                  = Defaults.LOG
    bench: str           = Defaults.BENCH
    iter: bool

    noncritical_delay: int
    groups: int
    critical_delay: int

    low_contention = Defaults.LOW_CONTENTION
    stagger_ms     = Defaults.STAGGER_MS
    skip_experiment: bool = False

    iter_threads: list[int] | None
    iter_critical_delay: list[int] | None
    iter_noncritical_delay: list[int] | None

