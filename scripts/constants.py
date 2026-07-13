# scripts/constants.py

import logging

class Constants:
    class Defaults:

        # =====================================================================
        # LOCK SERIES (building blocks — compose comparison sets from these)
        #
        # Status notes reflect the 2026-07 verification sweeps (breach-
        # detecting critical section, 5 reps x {2,4,8,12,16} threads; see
        # ELEVATOR_SET_COMPARISON.md §7.4-7.10). Locks commented out inside a
        # series are excluded for the stated, still-current reason.
        # =====================================================================

        # -- Unfair spin locks (throughput baselines; no ordering) -----------
        SPIN_SERIES = [
            "spin",
            "exp_spin",
            "hard_spin",
            # "wait_spin",  # sleep-based variant; excluded from defaults
        ]

        # -- Ticket locks (FIFO via a serving counter) ------------------------
        TICKET_SERIES = [
            "ticket",
            "ring_ticket",          # Anderson array lock (rewritten 2026-07-12)
            "threadlocal_ticket",
        ]

        # -- Queue locks (FIFO via per-thread nodes; hardware RMW) ------------
        QUEUE_SERIES = [
            "mcs",
            "mcs_nca",
            "mcs_local",
            "mcs_malloc",           # CAS expected-arg bug fixed 2026-07-12
            "clh",                  # note: one allocation per lock() — slow
        ]

        # -- Hierarchical / cohort locks (NUMA-structured) ---------------------
        HIERARCHICAL_SERIES = [
            "hmcs",
            "hclh",
            "cohortMCS",            # rewritten 2026-07-12 (cohort-owned global node)
            "cohortTicket",
            "cohortPTicket",
            "cohortTAS",
            "hbo",
        ]

        # -- Classic software mutual exclusion (no RMW atomics) ----------------
        SOFTWARE_CLASSIC_SERIES = [
            "dijkstra",
            "dijkstra_nonatomic",
            "bakery",
            "bakery_nonatomic",
            "boulangerie",
            "lamport",
            "knuth",
            "peterson",
            "szymanski",
            # "burns_lamport",  # passes current sweeps; fences added 2026-07-11
            # "yang",           # sometimes deadlocks (pre-existing, unfixed)
        ]

        # -- Elevator locks (designated-waker scan/tree) -----------------------
        ELEVATOR_SERIES = [
            "linear_cas_elevator",
            "linear_bl_elevator",
            "linear_lamport_elevator",
            "tree_cas_elevator",
            "tree_bl_elevator",
            "tree_lamport_elevator",   # passes all 2026-07 sweeps (old deadlock note stale)
            "elevator",
        ]
        ELEVATOR_NCA_SERIES = [        # non-cache-aligned variants
            "mcs_nca",
            "tree_cas_elevator_nca",
            "linear_cas_elevator_nca",
            "linear_bl_elevator_nca",
            "linear_lamport_elevator_nca",
            "tree_lamport_elevator_nca",
            # "tree_bl_elevator_nca",  # crashes (SIGBUS) on arm64 — pre-existing
        ]

        # -- Counting-network base locks (Herlihy Ch.12; O(n) unlock scan) -----
        NETWORK_BASE_SERIES = [
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
        ]

        # -- NEW DESIGNS: linearizable / overtaking counting locks -------------
        # (linearizable_counting_lock.hpp + net_elevator_lock.hpp; the
        #  designs this project contributes — see LINEARIZABLE_COUNTING_ANALYSIS.md)
        NEW_DESIGN_SERIES = [
            "net_elevator",         # network-assigned floors + elevator handoff
            "wf_bitonic_cas",       # Design C: waiting filter, O(1) unlock
            "wf_bitonic_bl",
            "wf_bitonic_lamport",
            "wf_bitonic_bakery",    # RMW-free + distributed + O(1) unlock
            "wf_periodic_cas",
            "wf_periodic_bl",
            "wf_periodic_lamport",
            "wf_periodic_bakery",
            "seq_bitonic_cas",      # Design B: global ticket (repaired 2026-07-11)
            "seq_periodic_cas",
            "lw_bitonic_cas",       # Design A: per-wire O(W) unlock (repaired 2026-07-11)
            "lw_bitonic_bakery",
            "lw_periodic_cas",
            "lw_periodic_bakery",
            "skew_bitonic_cas",     # ticket + filter-overhead model (see §4b)
            "skew_periodic_cas",
            "bo_bitonic_cas",       # Design F: bounded overtaking (2026-07-12)
            "bo_periodic_cas",
            "hb_bitonic_cas",       # Design G: heartbeat overtaking (2026-07-12)
            "hb_periodic_cas",
        ]

        # =====================================================================
        # COMPARISON SETS (select with -s <name>; see MUTEX_SETS below)
        # =====================================================================

        # THE headline comparison: every new design against the measured
        # champion of each classical series (champions per
        # ELEVATOR_SET_COMPARISON.md §7.8-7.10, this hardware):
        #   unfair spin ..... exp_spin           (absolute throughput ceiling)
        #   ticket .......... ticket             (robustness-per-line champion)
        #   queue ........... mcs                (best strict FIFO at 2-8T)
        #   hierarchical .... hclh               (best ordered at 12T+)
        #   cohort .......... cohortTicket       (best cohort variant)
        #   elevator ........ tree_lamport_elevator (best RMW-free elevator)
        #   elevator ........ linear_bl_elevator (RMW-free, best at 8T)
        #   network base .... periodic_cas       (best base CAS network)
        #   network base .... bitonic_bakery     (RMW-free network champion)
        CHAMPIONS_SET = [
            # classical champions
            "exp_spin",
            "ticket",
            "mcs",
            "hclh",
            "cohortTicket",
            "tree_lamport_elevator",
            "linear_bl_elevator",
            "periodic_cas",
            "bitonic_bakery",
            # new designs (one representative per design; add variants as needed)
            "net_elevator",
            "wf_bitonic_cas",
            "wf_bitonic_bakery",
            "seq_periodic_cas",
            "lw_bitonic_cas",
            "skew_bitonic_cas",
            "bo_bitonic_cas",
            "hb_bitonic_cas",
            "hb_periodic_cas",
        ]

        # Everything network-related vs the two key baselines.
        NETWORK_SET = (
            ["exp_spin", "mcs"]
            + NETWORK_BASE_SERIES
            + NEW_DESIGN_SERIES
        )

        # The historical elevator-group comparison (kept for continuity with
        # existing plots/data; now composed from series).
        ELEVATOR_SET = (
            ["exp_spin", "mcs"]
            + ELEVATOR_SERIES
            + NETWORK_BASE_SERIES
            + [
                "wf_bitonic_cas",
                "wf_bitonic_bl",
                "wf_bitonic_lamport",
                "wf_bitonic_bakery",
                "skew_bitonic_cas",
                "bo_bitonic_cas",
                "hb_bitonic_cas",
            ]
            + ELEVATOR_NCA_SERIES
        )

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
            # "yang", "yang_sleeper",  # sometimes deadlock (pre-existing)
        ]

        FENCING_SET = [
            "dijkstra",
            "dijkstra_nonatomic",
            "bakery",
            "bakery_nonatomic",
            "exp_spin",
            "hard_spin",
        ]

        # Broad kitchen-sink set (one of everything that works).
        BASE_SET = (
            SPIN_SERIES
            + ["ticket", "mcs", "clh", "hopscotch", "halfnode", "system", "nsync"]
            + SOFTWARE_CLASSIC_SERIES
            + ELEVATOR_SERIES
            + NETWORK_BASE_SERIES
            + [
                "net_elevator",
                "wf_bitonic_cas", "wf_bitonic_bl",
                "wf_periodic_cas", "wf_periodic_bl",
                "seq_bitonic_cas", "seq_periodic_cas",
                "lw_bitonic_cas", "lw_periodic_cas",
                "bo_bitonic_cas", "hb_bitonic_cas",
            ]
        )

        # Locks meaningful under CXL/NUMA allocation experiments.
        # note: CLH allocates per lock() — too slow; peterson very slow.
        CXL_SET = (
            [
                "bakery", "bakery_nonatomic", "boulangerie", "burns_lamport",
                "dijkstra", "dijkstra_nonatomic", "spin", "exp_spin",
                "knuth", "lamport", "mcs", "peterson", "ticket",
                "linear_cas_elevator", "linear_bl_elevator",
                "tree_cas_elevator", "tree_bl_elevator",
            ]
            + NETWORK_BASE_SERIES
            + [
                "net_elevator",
                "wf_bitonic_cas", "wf_bitonic_bl",
                "wf_bitonic_lamport", "wf_bitonic_bakery",
                "skew_bitonic_cas",
                "bo_bitonic_cas", "hb_bitonic_cas",
            ]
        )

        # RMW-free locks only (pure loads/stores/fences).
        SOFTWARE_CXL_SET = [
            "bakery_nonatomic",
            "lamport",
            "linear_bl_elevator",
            "linear_lamport_elevator",
            "tree_bl_elevator",
            "tree_lamport_elevator",
            "peterson",
            "knuth",
            "boulangerie",
            "bitonic_bakery",
            "wf_bitonic_bakery",
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
            "champions": CHAMPIONS_SET,   # new designs vs best-of-each-series
            "network": NETWORK_SET,
            "sleeper": SLEEPER_SET,
            "elevator": ELEVATOR_SET,
            "fencing": FENCING_SET,
            "base": BASE_SET,
            "cxl": CXL_SET,
            "software_cxl": SOFTWARE_CXL_SET,
            "hardware_cxl": HARDWARE_CXL_SET,
            "combined_cxl": COMBINED_CXL_SET,
        }

        # Default lock list when no -s/-i selection is given.
        MUTEX_NAMES = CHAMPIONS_SET

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
    capture: str = "latency"
    plot: str = "auto"
