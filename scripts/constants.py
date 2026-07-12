# scripts/constants.py

import logging

class Constants:
    class Defaults:

        MUTEX_NAMES = [
            # "burns_lamport", TODO: Fix (Kush saw a data race multiple times)
            "dijkstra",
            "dijkstra_nonatomic",
            # "dijkstra_nonatomic_sleeper",
            "bakery",
            "bakery_nonatomic",
            "spin",
            "exp_spin",
            # "wait_spin",
            # "nsync",
            # "system",
            "mcs",
            # "mcs_sleeper",
            "knuth",
            # "knuth_sleeper", Likely is not possible
            "peterson",
            "clh",
            "hopscotch",
            "ticket", 
            "halfnode", 
            "lamport", 
            # "lamport_sleeper", 
            "boulangerie",
            "tree_cas_elevator",
            "linear_cas_elevator", #TODO: fix (Kush saw a data race once)
            "tree_bl_elevator",
            "linear_bl_elevator",  #TODO: fix (Kush saw a data race once)
            "linear_lamport_elevator", #TODO: fix (Kush saw a data race multiple times)
            "tree_lamport_elevator", #TODO: fix trees (known to deadlock)
            # "futex", TODO: Add guard for macOS
            "szymanski",
            "hard_spin",
            "net_elevator",
            "bitonic_cas",
            "bitonic_bl",
            "bitonic_lamport",
            "bitonic_elevator",
            "bitonic_bakery",
            "periodic_cas",
            "periodic_bl",
            "periodic_lamport",
            "periodic_elevator",
            "periodic_bakery",
            "wf_bitonic_cas",
            "wf_bitonic_bl",
            "wf_bitonic_lamport",
            "wf_bitonic_bakery",
            "bo_bitonic_cas",
            "bo_periodic_cas",
            "hb_bitonic_cas",
            "hb_periodic_cas",
            "wf_periodic_cas",
            "wf_periodic_bl",
            "wf_periodic_lamport",
            "wf_periodic_bakery",
            "skew_bitonic_cas",
            "skew_periodic_cas",
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
            "net_elevator",
            "bitonic_cas",
            "bitonic_bl",
            "bitonic_lamport",
            "bitonic_elevator",
            "bitonic_bakery",
            "periodic_cas",
            "periodic_bl",
            "periodic_lamport",
            "periodic_elevator",
            "periodic_bakery",
            # "lw_bitonic_cas", These do not work
            # "lw_bitonic_bl",
            # "lw_periodic_cas",
            # "lw_periodic_bl",
            # "seq_bitonic_cas",
            # "seq_periodic_cas",
            "wf_bitonic_cas",
            "wf_bitonic_bl",
            "wf_bitonic_lamport",
            "wf_bitonic_bakery",
            # "wf_periodic_cas",
            # "wf_periodic_bl",
            # # "wf_periodic_lamport",
            # "wf_periodic_bakery",
            "skew_bitonic_cas",
            # "skew_periodic_cas",

            "mcs_nca",
            "tree_cas_elevator_nca",
            "linear_cas_elevator_nca", #TODO: fix (Kush saw a data race once)
            # "tree_bl_elevator_nca",
            "linear_bl_elevator_nca",  #TODO: fix (Kush saw a data race once)
            "linear_lamport_elevator_nca", #TODO: fix (Kush saw a data race multiple times)
            "tree_lamport_elevator_nca", #TODO: fix trees (known to deadlock)
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
            # "tree_bl_elevator",
            "linear_bl_elevator",  #TODO: fix (Kush saw a data race once)
            "linear_lamport_elevator", #TODO: fix (Kush saw a data race multiple times)
            "tree_lamport_elevator", #TODO: fix trees (known to deadlock)
            # "futex", TODO: Add guard for macOS
            "szymanski",
            "net_elevator",
            "bitonic_cas",
            "bitonic_bl",
            "bitonic_lamport",
            "bitonic_elevator",
            "bitonic_bakery",
            "periodic_cas",
            "periodic_bl",
            "periodic_lamport",
            "periodic_elevator",
            "periodic_bakery",
            "lw_bitonic_cas",
            "lw_bitonic_bl",
            "lw_periodic_cas",
            "lw_periodic_bl",
            "seq_bitonic_cas",
            "seq_periodic_cas",
            "wf_bitonic_cas",
            "wf_bitonic_bl",
            "wf_periodic_cas",
            "wf_periodic_bl",
            # "yang", TODO: Sometimes deadlocks
        ]


        # note: CLH technically works but it makes one allocation per
        # lock operation, so it is too slow.
        # peterson is also probaby really really slow
        CXL_SET = [
            "bakery",
            "bakery_nonatomic",
            "boulangerie",
            "burns_lamport",
            "dijkstra",
            "dijkstra_nonatomic",
            "spin",
            "exp_spin",
            # "hopscotch",
            "knuth",
            "lamport",
            "linear_cas_elevator",
            "linear_bl_elevator",
            "tree_cas_elevator",
            "tree_bl_elevator",
            "mcs",
            "peterson",
            "ticket",
            "net_elevator",
            "bitonic_cas",
            "bitonic_bl",
            "bitonic_lamport",
            "bitonic_elevator",
            "bitonic_bakery",
            "periodic_cas",
            "periodic_bl",
            "periodic_lamport",
            "periodic_elevator",
            "periodic_bakery",
            "wf_bitonic_cas",
            "wf_bitonic_bl",
            "wf_bitonic_lamport",
            "wf_bitonic_bakery",
            "skew_bitonic_cas",
        ]

        SOFTWARE_CXL_SET = [
            "bakery_nonatomic",
        #    "burns_lamport",
           "lamport",
            "linear_bl_elevator",
           "linear_lamport_elevator",
            "tree_bl_elevator",
           "tree_lamport_elevator",
            "peterson",
            "knuth",
            "boulangerie",
        ]

        HARDWARE_CXL_SET = [
            "spin",
            "exp_spin",
            "ticket",
            # "mcs",
            "mcs_local",
        ]

        COMBINED_CXL_SET = SOFTWARE_CXL_SET + HARDWARE_CXL_SET
        MUTEX_SETS = {
            "sleeper": SLEEPER_SET,
            "elevator": ELEVATOR_SET,
            "fencing": FENCING_SET,
            "base": BASE_SET,
            "cxl": CXL_SET,
            "software_cxl": SOFTWARE_CXL_SET,
            "hardware_cxl": HARDWARE_CXL_SET,
            "combined_cxl": COMBINED_CXL_SET,
        }

        CONDITIONAL_COMPILATION_MUTEXES = [
            "nsync",
            "boost",
            "umwait",
            "futex",
        ]

        EXECUTABLE_NAME = "max_contention_bench"
        BENCH_EXECUTABLES = {
            "max": "./build/apps/max_contention_bench/max_contention_bench",
            "grouped": "./build/apps/grouped_contention_bench/grouped_contention_bench",
            "min": "./build/apps/min_contention_bench/min_contention_bench",
        }
        BENCH_N_THREADS = 10
        BENCH_N_SECONDS = 1

        N_PROGRAM_ITERATIONS = 10
        DATA_FOLDER          = "./data/generated"
        LOGS_FOLDER          = "./data/logs"
        EXECUTABLE           = BENCH_EXECUTABLES["max"]
        MULTITHREADED        = False
        THREAD_LEVEL         = False
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
    software_cxl: bool
    hardware_cxl: bool
    skip_plotting: bool
    show: bool = False
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
    faceted = None  # None = auto, True = force faceted, False = force combined
    variability: bool = False
    speedup: bool = False
    speedup_ref: str = 'exp_spin'
