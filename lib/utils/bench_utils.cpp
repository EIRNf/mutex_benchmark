#include "bench_utils.hpp"
#include "../lock/lock.hpp"

#include <stdio.h>
#include <cstdlib>
#include <vector>
#include <string>
#include <string.h>

#include "../lock/pthread_lock.cpp"
#include "../lock/cpp_std_mutex.cpp"
#include "../lock/boost_lock.cpp"
#include "../lock/dijkstra_lock.cpp"
#include "../lock/dijkstra_nonatomic_lock.cpp"
#include "../lock/spin_lock.hpp"
#include "../lock/nsync_lock.cpp"
#include "../lock/exp_spin_lock.cpp"
#include "../lock/bakery_mutex.cpp"
#include "../lock/bakery_nonatomic_mutex.cpp"
#include "../lock/lamport_lock.cpp"
#include "../lock/mcs_lock.cpp"
#include "../lock/mcs_volatile_lock.cpp"
#include "../lock/mcs_malloc_lock.cpp"
#include "../lock/knuth_lock.cpp"
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
#include "../lock/burns_lamport_lock.hpp"
#include "../lock/futex_mutex.cpp"
#include "../lock/elevator_mutex.hpp"
#include "../lock/szymanski.cpp"

void record_rusage() {
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        print_rusage(&usage);
    } else {
        perror("getrusage failed");
    }
}

void print_rusage(struct rusage *usage) {
    printf("User CPU time used: %ld.%06ld seconds\n", 
           usage->ru_utime.tv_sec, usage->ru_utime.tv_usec);
    printf("System CPU time used: %ld.%06ld seconds\n", 
           usage->ru_stime.tv_sec, usage->ru_stime.tv_usec);
    printf("Maximum resident set size: %ld KB\n", usage->ru_maxrss);
    printf("Integral shared memory size: %ld KB\n", usage->ru_ixrss);
    printf("Integral unshared data size: %ld KB\n", usage->ru_idrss);
    printf("Integral unshared stack size: %ld KB\n", usage->ru_isrss);
    printf("Page reclaims (soft page faults): %ld\n", usage->ru_minflt);
    printf("Page faults (hard page faults): %ld\n", usage->ru_majflt);
    printf("Swaps: %ld\n", usage->ru_nswap);
    printf("Block input operations: %ld\n", usage->ru_inblock);
    printf("Block output operations: %ld\n", usage->ru_oublock);
    printf("IPC messages sent: %ld\n", usage->ru_msgsnd);
    printf("IPC messages received: %ld\n", usage->ru_msgrcv);
    printf("Signals received: %ld\n", usage->ru_nsignals);
    printf("Voluntary context switches: %ld\n", usage->ru_nvcsw);
    printf("Involuntary context switches: %ld\n", usage->ru_nivcsw);
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
    SoftwareMutex* lock = nullptr;
    if      (strcmp(mutex_name, "pthread") == 0)             lock = new Pthread();
    else if (strcmp(mutex_name, "cpp_std") == 0)             lock = new CPPMutex();
    else if (strcmp(mutex_name, "boost") == 0)               lock = new BoostMutex();
    else if (strcmp(mutex_name, "dijkstra") == 0)            lock = new DijkstraMutex();
    else if (strcmp(mutex_name, "dijkstra_nonatomic") == 0)  lock = new DijkstraNonatomicMutex();
    else if (strcmp(mutex_name, "spin") == 0)                lock = new SpinLock();
    else if (strcmp(mutex_name, "exp_spin") == 0)            lock = new ExponentialSpinLock();
    else if (strcmp(mutex_name, "nsync") == 0)               lock = new NSync();
    else if (strcmp(mutex_name, "bakery") == 0)              lock = new BakeryMutex();
    else if (strcmp(mutex_name, "bakery_nonatomic") == 0)    lock = new BakeryNonAtomicMutex();
    else if (strcmp(mutex_name, "lamport") == 0)             lock = new LamportLock();
    else if (strcmp(mutex_name, "mcs") == 0)                 lock = new MCSMutex();
    else if (strcmp(mutex_name, "mcs_volatile") == 0)        lock = new MCSVolatileMutex();
    else if (strcmp(mutex_name, "mcs_malloc") == 0)          lock = new MCSMallocMutex();
    else if (strcmp(mutex_name, "knuth") == 0)               lock = new KnuthMutex();
    else if (strcmp(mutex_name, "peterson") == 0)            lock = new PetersonMutex();
    else if (strcmp(mutex_name, "boulangerie") == 0)         lock = new Boulangerie();
    else if (strcmp(mutex_name, "szymanski") == 0)           lock = new SzymanskiLock();
    else if (strcmp(mutex_name, "ticket") == 0)              lock = new TicketMutex();
    else if (strcmp(mutex_name, "threadlocal_ticket") == 0)  lock = new ThreadlocalTicketMutex();
    else if (strcmp(mutex_name, "ring_ticket") == 0)         lock = new RingTicketMutex();
    else if (strcmp(mutex_name, "null") == 0)                lock = new NullMutex();
    else if (strcmp(mutex_name, "halfnode") == 0)            lock = new HalfnodeMutex();
    else if (strcmp(mutex_name, "hopscotch") == 0)           lock = new HopscotchMutex();
    else if (strcmp(mutex_name, "clh") == 0)                 lock = new CLHMutex();
    else if (strcmp(mutex_name, "linear_cas_elevator") == 0) lock = new LinearCASElevatorMutex();
    else if (strcmp(mutex_name, "tree_cas_elevator") == 0)   lock = new TreeCASElevatorMutex();
    else if (strcmp(mutex_name, "linear_bl_elevator") == 0)  lock = new LinearBLElevatorMutex();
    else if (strcmp(mutex_name, "tree_bl_elevator") == 0)    lock = new TreeBLElevatorMutex();
    else if (strcmp(mutex_name, "burns_lamport") == 0)       lock = new BurnsLamportMutex();
    else if (strcmp(mutex_name, "futex") == 0)               lock = new FutexLock();
    else if (strcmp(mutex_name, "elevator") == 0)            lock = new ElevatorMutex();
    else if (strcmp(mutex_name, "hopscotch_static") == 0) {
        // This causes a free / delete / delete[] mismatch
        size_t region_size = HopscotchStaticMutex::get_size(num_threads);
        void *region = malloc(region_size);
        lock = new (region) HopscotchStaticMutex();
    } else {
        fprintf(stderr,
            "Unrecognized mutex '%s'\n", mutex_name
        );
        return nullptr;
    }
    return lock;
}

void cleanup_mutex(SoftwareMutex *mutex) {
    // if (mutex->name().compare("hopscotch_static") == 0) {
        // printf("Cleaning up...\n");
        // free(static_cast<void*>(mutex));
    // }
}