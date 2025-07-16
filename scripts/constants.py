# scripts/constants.py

import logging

class Constants:
    class Defaults:

        MUTEX_NAMES          = [
            "dijkstra_nonatomic", "dijkstra_nonatomic_sleeper", "bakery_nonatomic", "exp_spin", "wait_spin", "nsync", "system",
            "mcs", "mcs_sleeper", "knuth", "peterson", "lamport", "lamport_sleeper", "boulangerie", "szymanski"
        ]
        EXECUTABLE_NAME      = "max_contention_bench"
        BENCH_N_THREADS      = "10"
        BENCH_N_SECONDS      = "1"

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

        LOW_CONTENTION = False
        STAGGER_MS     = 0

    mutex_names          = Defaults.MUTEX_NAMES
    bench_n_threads      = Defaults.BENCH_N_THREADS
    bench_n_seconds      = Defaults.BENCH_N_SECONDS
    n_program_iterations = Defaults.N_PROGRAM_ITERATIONS
    data_folder          = Defaults.DATA_FOLDER
    logs_folder          = Defaults.LOGS_FOLDER
    executable           = Defaults.EXECUTABLE
    multithreaded        = Defaults.MULTITHREADED
    thread_level         = Defaults.THREAD_LEVEL
    scatter              = Defaults.SCATTER
    max_n_points         = Defaults.MAX_N_POINTS
    noncritical_delay    = Defaults.SKIP
    log                  = Defaults.LOG

    low_contention = Defaults.LOW_CONTENTION
    stagger_ms     = Defaults.STAGGER_MS

    iter_v_threads = None
    threads_start  = None
    threads_end    = None
    threads_step   = None
