# scripts/constants.py

import logging

class Constants:
    class Defaults:

        MUTEX_NAMES = [
            # "burns_lamport", TODO: Fix (Kush saw a data race multiple times)
            "dijkstra",
            "dijkstra_nonatomic",
            "dijkstra_nonatomic_sleeper",
            "bakery",
            "bakery_nonatomic",
            "spin",
            "exp_spin",
            "wait_spin",
            "nsync",
            "system",
            "mcs",
            "mcs_sleeper",
            "knuth",
            # "knuth_sleeper", Likely is not possible
            "peterson",
            "clh",
            "hopscotch",
            "ticket", 
            "halfnode", 
            "lamport", 
            "lamport_sleeper", 
            "boulangerie",
            "tree_cas_elevator",
            "linear_cas_elevator", #TODO: fix (Kush saw a data race once)
            "tree_bl_elevator",
            "linear_bl_elevator",  #TODO: fix (Kush saw a data race once)
            "linear_lamport_elevator", #TODO: fix (Kush saw a data race multiple times)
            "tree_lamport_elevator", #TODO: fix trees (known to deadlock)
            # "futex", TODO: Add guard for macOS
            "szymanski",
            "hard_spin"
            # "yang", TODO: Sometimes deadlocks
            # "yang_sleeper", TODO: Sometimes deadlocks
        ]

        # MUTEX COMPARISON SETS (exp_spin is used as a baseline for all)
        SLEEPER_SET = [
            "dijkstra_nonatomic",
            "dijkstra_nonatomic_sleeper",
            "spin",
            "exp_spin",
            "wait_spin",
            "system",
            "mcs",
            "mcs_sleeper",
            "lamport", 
            "lamport_sleeper", 
            # "yang", TODO: Sometimes deadlocks
            # "yang_sleeper", TODO: Sometimes deadlocks
        ]

        ELEVATOR_SET = [
            "exp_spin",
            "mcs",
            "tree_cas_elevator",
            "linear_cas_elevator", #TODO: fix (Kush saw a data race once)
            "tree_bl_elevator",
            "linear_bl_elevator",  #TODO: fix (Kush saw a data race once)
            "linear_lamport_elevator", #TODO: fix (Kush saw a data race multiple times)
            "tree_lamport_elevator", #TODO: fix trees (known to deadlock)
        ]

        FENCING_SET = [
            "dijkstra",
            "dijkstra_nonatomic",
            "bakery",
            "bakery_nonatomic",
            "exp_spin",
            "hard_spin"
        ]

        BASE_SET = [
            # "burns_lamport", TODO: Fix (Kush saw a data race multiple times)
            "dijkstra",
            "bakery",
            "spin",
            "exp_spin",
            "hard_spin",
            "nsync",
            "system",
            "mcs",
            "knuth",
            "peterson",
            "clh",
            "hopscotch",
            "ticket", 
            "halfnode", 
            "lamport", 
            "boulangerie",
            "tree_cas_elevator",
            "linear_cas_elevator", #TODO: fix (Kush saw a data race once)
            "tree_bl_elevator",
            "linear_bl_elevator",  #TODO: fix (Kush saw a data race once)
            "linear_lamport_elevator", #TODO: fix (Kush saw a data race multiple times)
            "tree_lamport_elevator", #TODO: fix trees (known to deadlock)
            # "futex", TODO: Add guard for macOS
            "szymanski",
            # "yang", TODO: Sometimes deadlocks
        ]




        # note: CLH technically works but it makes one allocation per
        # lock operation, so it is too slow.
        # peterson is also probaby really really slow
        CXL_MUTEXES = [
            "bakery",
            "bakery_nonatomic",
            "boulangerie",
            "burns_lamport",
            "dijkstra",
            "dijkstra_nonatomic",
            "spin",
            "exp_spin",
            "hopscotch",
            "knuth",
            "lamport",
            "linear_cas_elevator",
            "linear_bl_elevator",
            "tree_cas_elevator",
            "tree_bl_elevator",
            "mcs",
            "peterson",
            "szymanski",
            "ticket",
        ]

        CONDITIONAL_COMPILATION_MUTEXES = [
            "nsync",
            "boost",
            "umwait",
            "futex",
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
        STANDARD_DEVIATION_SCALE = 1.0

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
    rusage: bool
    cxl: bool
    skip_plotting: bool
    averages: bool
    iter_variable_name: str
    stdev_scale: float

    noncritical_delay: int
    groups: int
    critical_delay: int

    low_contention = Defaults.LOW_CONTENTION
    stagger_ms     = Defaults.STAGGER_MS
    skip_experiment: bool = False
    iter_range: list[int]
