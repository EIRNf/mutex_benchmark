#include "bench_utils.hpp"
#include "../lock/lock.hpp"

#include <stdio.h>
#include <cstdlib>
#include <iostream>
#include <vector>
#include <string>
#include <string.h>


#include "../lock/system_lock.cpp"
#include "../lock/cpp_std_mutex.cpp"
#include "../lock/boost_lock.cpp"
#include "../lock/dijkstra_lock.cpp"
#include "../lock/dijkstra_nonatomic_lock.cpp"
#include "../lock/dijkstra_nonatomic_sleeper_lock.cpp"
#include "../lock/spin_lock.hpp"
#include "../lock/nsync_lock.cpp"
#include "../lock/exp_spin_lock.cpp"
#include "../lock/wait_spin_lock.cpp"
#include "../lock/bakery_mutex.cpp"
#include "../lock/bakery_nonatomic_mutex.cpp"
#include "../lock/lamport_lock.cpp"
#include "../lock/lamport_sleeper_lock.cpp"
#include "../lock/mcs_lock.cpp"
#include "../lock/mcs_sleeper_lock.cpp"
#include "../lock/mcs_volatile_lock.cpp"
#include "../lock/mcs_malloc_lock.cpp"
#include "../lock/knuth_lock.cpp"
#include "../lock/knuth_sleeper_lock.cpp"
#include "../lock/peterson_lock.cpp"
#include "../lock/boulangerie.cpp"
#include "../lock/ticket_lock.cpp"
#include "../lock/threadlocal_ticket_lock.cpp"
#include "../lock/ring_ticket_lock.cpp"
#include "../lock/null_mutex.cpp"
#include "../lock/halfnode_lock.cpp"
#include "../lock/hopscotch_lock.cpp"
#include "../lock/hopscotch_static_lock.cpp"
#include "../lock/clh_lock.cpp"
#include "../lock/linear_cas_elevator.cpp"
#include "../lock/tree_cas_elevator.cpp"
#include "../lock/linear_bl_elevator.cpp"
#include "../lock/tree_bl_elevator.cpp"
#include "../lock/linear_lamport_elevator.cpp"
#include "../lock/tree_lamport_elevator.cpp"
#include "../lock/burns_lamport_lock.hpp"
// #include "../lock/futex_mutex.cpp"
#include "../lock/elevator_mutex.hpp"
#include "../lock/szymanski.cpp"
#include "../lock/yang_lock.cpp"
#include "../lock/yang_sleeper_lock.cpp"
#include "../lock/hardspin_lock.hpp"

void record_rusage(bool csv) {
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        print_rusage(&usage, csv);
    } else {
        perror("getrusage failed");
    }
}

void print_rusage(struct rusage *usage, bool csv) {
    if (!csv){
        printf("User CPU time used: %ld.%06ld seconds\n", 
            usage->ru_utime.tv_sec, usage->ru_utime.tv_usec); //slightly important, not as much
        printf("System CPU time used: %ld.%06ld seconds\n", 
            usage->ru_stime.tv_sec, usage->ru_stime.tv_usec); //important and relevant
        printf("Maximum resident set size: %ld KB\n", usage->ru_maxrss); //not too important?
        printf("Integral shared memory size: %ld KB\n", usage->ru_ixrss); //unmantained
        printf("Integral unshared data size: %ld KB\n", usage->ru_idrss); //unmantained
        printf("Integral unshared stack size: %ld KB\n", usage->ru_isrss); //unmantained
        printf("Page reclaims (soft page faults): %ld\n", usage->ru_minflt); //slightly important
        printf("Page faults (hard page faults): %ld\n", usage->ru_majflt); //slightly important
        printf("Swaps: %ld\n", usage->ru_nswap); //unmantained 
        printf("Block input operations: %ld\n", usage->ru_inblock); //just linux
        printf("Block output operations: %ld\n", usage->ru_oublock); //just linux
        printf("IPC messages sent: %ld\n", usage->ru_msgsnd); //unmantained
        printf("IPC messages received: %ld\n", usage->ru_msgrcv); //unmantained
        printf("Signals received: %ld\n", usage->ru_nsignals); //unmantained
        printf("Voluntary context switches: %ld\n", usage->ru_nvcsw); //just linux
        printf("Involuntary context switches: %ld\n", usage->ru_nivcsw); //just linux
    }
    else{
        printf("%ld.%06ld,%ld.%06ld,%ld,%ld,%ld", usage->ru_utime.tv_sec, usage->ru_utime.tv_usec, usage->ru_stime.tv_sec, usage->ru_stime.tv_usec, usage->ru_maxrss, usage->ru_minflt, usage->ru_majflt);
    }
}

void init_lock_timer(struct per_thread_stats *stats) {
    // Currently unused.
    (void)stats;
}

void start_lock_timer(struct per_thread_stats *stats) {
    clock_gettime(CLOCK_MONOTONIC, &stats->start_time);
}

double get_elapsed_time(struct timespec start_time, struct timespec end_time) {
    long seconds = end_time.tv_sec - start_time.tv_sec;
    long nanoseconds = end_time.tv_nsec - start_time.tv_nsec;

    if (nanoseconds < 0) {
        seconds--;
        nanoseconds += 1e9;
    }
    double elapsed = seconds + nanoseconds / 1e9;
    if (elapsed < 0.0) {
        return 0.0;
    }
    return elapsed;
}

void end_lock_timer(struct per_thread_stats *stats) {
    clock_gettime(CLOCK_MONOTONIC, &stats->end_time);
    double lock_time = get_elapsed_time(stats->start_time, stats->end_time);
    stats->lock_times.push_back(lock_time);
}

void destroy_lock_timer(struct per_thread_stats *stats) {
    (void)stats; // Currently
}

void report_thread_latency(struct per_thread_stats *stats, bool csv, bool thread_level) {
    if (thread_level) {
        if (csv) {
            // Thread ID, Runtime, # Iterations
            printf("%d,%f,%d\n", stats->thread_id, stats->run_time, stats->num_iterations);
        } else {

            printf("Thread %d: %d iterations completed in %f seconds\n",
                stats->thread_id, stats->num_iterations, stats->run_time);

        }
    } else {
        if (csv) {
            for (int i = 0; i < stats->num_iterations; i++) {
                // Thread ID, Iteration #, Time to lock
                printf("%d,%d,%.10f\n", stats->thread_id, i, stats->lock_times[i]);
            }
        }
        else {
            printf("Thread %d: %d iterations completed in %f seconds\n",
                stats->thread_id, stats->num_iterations, stats->run_time);
            for (int i = 0; i < stats->num_iterations; i++) {
                // Thread ID, Iteration #, Time to lock
                printf("    #%d: iteration %d took %.9f seconds\n", stats->thread_id, i, stats->lock_times[i]);
            }
        }
    }
}

void report_run_latency(struct run_args *stats){
    printf("Run statistics:\n");
    (void)stats;
}

void busy_sleep(size_t iterations) {
    volatile size_t i;
    for (i = 0; i < iterations; i++);
}

SoftwareMutex *get_mutex(const char *mutex_name, size_t num_threads) {
    (void)num_threads; // May be used in the future

    SoftwareMutex* lock = nullptr;
    if      (strcmp(mutex_name, "system") == 0)                      lock = new System();
    else if (strcmp(mutex_name, "cpp_std") == 0)                     lock = new CPPMutex();
    else if (strcmp(mutex_name, "boost") == 0)                       lock = new BoostMutex();
    else if (strcmp(mutex_name, "dijkstra") == 0)                    lock = new DijkstraMutex();
    else if (strcmp(mutex_name, "dijkstra_nonatomic") == 0)          lock = new DijkstraNonatomicMutex();
    else if (strcmp(mutex_name, "dijkstra_nonatomic_sleeper") == 0)  lock = new DijkstraNonatomicSleeperMutex();
    else if (strcmp(mutex_name, "spin") == 0)                        lock = new SpinLock();
    else if (strcmp(mutex_name, "hard_spin") == 0)                   lock = new HardSpinLock();
    else if (strcmp(mutex_name, "exp_spin") == 0)                    lock = new ExponentialSpinLock();
    else if (strcmp(mutex_name, "wait_spin") == 0)                   lock = new WaitSpinLock();
    else if (strcmp(mutex_name, "nsync") == 0)                       lock = new NSync();
    else if (strcmp(mutex_name, "bakery") == 0)                      lock = new BakeryMutex();
    else if (strcmp(mutex_name, "bakery_nonatomic") == 0)            lock = new BakeryNonAtomicMutex();
    else if (strcmp(mutex_name, "lamport") == 0)                     lock = new LamportLock();
    else if (strcmp(mutex_name, "lamport_sleeper") == 0)             lock = new LamportSleeperLock();
    else if (strcmp(mutex_name, "mcs") == 0)                         lock = new MCSMutex();
    else if (strcmp(mutex_name, "mcs_sleeper") == 0)                 lock = new MCSSleeperMutex();
    else if (strcmp(mutex_name, "mcs_volatile") == 0)                lock = new MCSVolatileMutex();
    else if (strcmp(mutex_name, "mcs_malloc") == 0)                  lock = new MCSMallocMutex();
    else if (strcmp(mutex_name, "knuth") == 0)                       lock = new KnuthMutex();
    else if (strcmp(mutex_name, "knuth_sleeper") == 0)               lock = new KnuthSleeperMutex();
    else if (strcmp(mutex_name, "peterson") == 0)                    lock = new PetersonMutex();
    else if (strcmp(mutex_name, "boulangerie") == 0)                 lock = new Boulangerie();
    else if (strcmp(mutex_name, "szymanski") == 0)                   lock = new SzymanskiLock();
    else if (strcmp(mutex_name, "ticket") == 0)                      lock = new TicketMutex();
    else if (strcmp(mutex_name, "threadlocal_ticket") == 0)          lock = new ThreadlocalTicketMutex();
    else if (strcmp(mutex_name, "ring_ticket") == 0)                 lock = new RingTicketMutex();
    else if (strcmp(mutex_name, "null") == 0)                        lock = new NullMutex();
    else if (strcmp(mutex_name, "halfnode") == 0)                    lock = new HalfnodeMutex();
    else if (strcmp(mutex_name, "hopscotch") == 0)                   lock = new HopscotchMutex();
    else if (strcmp(mutex_name, "clh") == 0)                         lock = new CLHMutex();
    else if (strcmp(mutex_name, "linear_cas_elevator") == 0)         lock = new LinearCASElevatorMutex();
    else if (strcmp(mutex_name, "tree_cas_elevator") == 0)           lock = new TreeCASElevatorMutex();
    else if (strcmp(mutex_name, "linear_bl_elevator") == 0)          lock = new LinearBLElevatorMutex();
    else if (strcmp(mutex_name, "tree_bl_elevator") == 0)            lock = new TreeBLElevatorMutex();
    else if (strcmp(mutex_name, "linear_lamport_elevator") == 0)     lock = new LinearLamportElevatorMutex();
    else if (strcmp(mutex_name, "tree_lamport_elevator") == 0)       lock = new TreeLamportElevatorMutex();
    else if (strcmp(mutex_name, "burns_lamport") == 0)               lock = new BurnsLamportMutex();
    else if (strcmp(mutex_name, "elevator") == 0)                    lock = new ElevatorMutex();
    else if (strcmp(mutex_name, "yang") == 0)                        lock = new YangMutex();
    else if (strcmp(mutex_name, "yang_sleeper") == 0)                lock = new YangSleeperMutex();
    // else if (strcmp(mutex_name, "hopscotch_static") == 0) {
    //     // This causes a free / delete / delete[] mismatch
    //     size_t region_size = HopscotchStaticMutex::get_size(num_threads);
    //     void *region = malloc(region_size);
    //     lock = new (region) HopscotchStaticMutex();
    // } 
    else {
        fprintf(stderr,
            "Unrecognized mutex '%s'\n", mutex_name
        );
        return nullptr;
    }
    return lock;
}