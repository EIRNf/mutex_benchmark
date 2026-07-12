#include "bench_utils.hpp"
#include "../lock/lock.hpp"

#include <stdio.h>
#include <cstdlib>
#include <iostream>
#include <vector>
#include <string>
#include <string.h>
#include <unordered_map>
#include <functional>
#ifdef __linux__
#include <numa.h>
#include <numaif.h>
#endif
#include <new>
#include <sys/mman.h>
#include <errno.h>


#include "../lock/system_lock.cpp"
#include "../lock/cpp_std_mutex.cpp"
#include "../lock/dijkstra_lock.cpp"
#include "../lock/dijkstra_nonatomic_lock.cpp"
#include "../lock/dijkstra_nonatomic_sleeper_lock.cpp"
#include "../lock/spin_lock.hpp"
#include "../lock/exp_spin_lock.cpp"
#include "../lock/wait_spin_lock.cpp"
#include "../lock/bakery_mutex.cpp"
#include "../lock/bakery_nonatomic_mutex.cpp"
#include "../lock/lamport_lock.hpp"
#include "../lock/lamport_sleeper_lock.cpp"
#include "../lock/mcs_lock.cpp"
#include "../lock/mcs_noncachealigned_lock.cpp"
#include "../lock/mcs_sleeper_lock.cpp"
#include "../lock/mcs_local_lock.cpp"
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
#include "../lock/hopscotch_noncachealigned_lock.cpp"
#include "../lock/hopscotch_local_lock.cpp"
#include "../lock/clh_lock.cpp"
#include "../lock/linear_elevator.cpp"
#include "../lock/tree_elevator.cpp"
#include "../lock/linear_elevator_nca.cpp"
#include "../lock/tree_elevator_nca.cpp"
#include "../lock/burns_lamport_lock.hpp"
// #include "../lock/futex_mutex.cpp"
#include "../lock/elevator_mutex.hpp"
#include "../lock/net_elevator_lock.hpp"
#include "../lock/bitonic_networks.hpp"
#include "../lock/linearizable_counting_lock.hpp"
#include "../lock/szymanski.cpp"
#include "../lock/broken_lock.cpp"
#include "../lock/yang_lock.cpp"
#include "../lock/yang_sleeper_lock.cpp"
#include "../lock/hardspin_lock.hpp"
#include "../lock/cohortTicket_lock.cpp"
#include "../lock/cohortMCS_lock.cpp"
#include "../lock/hbo_lock.cpp"
#include "../lock/cohortTAS_lock.cpp"
#include "../lock/cohortPTicket_lock.cpp"
#include "../lock/HCLH_lock.cpp"
#include "../lock/HMCS_lock.cpp"

#ifdef inc_nsync
    #include "../lock/nsync_lock.cpp"
#endif

#ifdef inc_boost
    #include "../lock/boost_lock.cpp"
#endif

#ifdef inc_umwait
    #include "../lock/umwait_lock.cpp"
#endif

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
        printf("User CPU time used: %ld.%06d seconds\n",
            usage->ru_utime.tv_sec, usage->ru_utime.tv_usec); //slightly important, not as much
        printf("System CPU time used: %ld.%06d seconds\n",
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
        printf("%ld.%06d,%ld.%06d,%ld,%ld,%ld", usage->ru_utime.tv_sec, usage->ru_utime.tv_usec, usage->ru_stime.tv_sec, usage->ru_stime.tv_usec, usage->ru_maxrss, usage->ru_minflt, usage->ru_majflt);
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
    for (i = 0; i < iterations; i+=1);
}


// Global variable to track mutex size for deallocation
static size_t g_mutex_alloc_size = 0;

#ifdef __linux__
// NUMA-aware delete for mutex objects
void numa_delete(SoftwareMutex* ptr) {
    if (ptr == nullptr) return;

    // Call destructor
    ptr->~SoftwareMutex();

    // Free memory using munmap (matches mmap)
    if (g_mutex_alloc_size > 0) {
        munmap(ptr, g_mutex_alloc_size);
    } else {
        free(ptr);  // Fallback
    }
}

// NUMA-aware placement new allocator for mutex objects
template<typename T>
T* numa_new() {
    void* mem = nullptr;
    size_t alloc_size = sizeof(T);

    // Use mmap directly to avoid brk() issues with NUMA
    mem = mmap(nullptr, alloc_size,
               PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS,
               -1, 0);

    if (mem == MAP_FAILED) {
        throw std::bad_alloc();
    }

    // Force bind to node 2
    unsigned long nodemask[16] = {0};
    unsigned long maxnode = sizeof(nodemask) * 8;

    // Set bit for node 2
    nodemask[0] = 1UL << 2;  // Node 2

    // Apply BIND policy to node 2
    if (mbind(mem, alloc_size, MPOL_BIND, nodemask, maxnode, MPOL_MF_MOVE) != 0) {
        fprintf(stderr, "mbind to node 2 failed: %s\n", strerror(errno));
    } else {
        fprintf(stderr, "Successfully bound %zu bytes at %p to node 2\n", alloc_size, mem);
     }

    // Touch all pages to force allocation according to policy
    memset(mem, 0, alloc_size);

    // Track the size for later deallocation
    g_mutex_alloc_size = alloc_size;

    // Use placement new to construct the object
    return new(mem) T();
}
#endif // __linux__


// Single name -> factory table, replacing the two previously-duplicated
// ~270-line strcmp chains (one for hardware_cxl via numa_new<T>(), one for
// everything else via new T()). The allocation strategy is now factored out
// once via the MK() macro instead of being repeated per lock name.
#ifdef hardware_cxl
    #define MK(Type) []() -> SoftwareMutex* { return numa_new<Type>(); }
#else
    #define MK(Type) []() -> SoftwareMutex* { return new Type(); }
#endif

static const std::unordered_map<std::string, std::function<SoftwareMutex*()>> kMutexTable = {
    {"hopscotch_local", MK(HopscotchLocalMutex)},
    {"clh", MK(CLHMutex)},
    {"elevator", MK(ElevatorMutex)},
    {"broken", MK(BrokenLock)},
    {"system", MK(System)},
    {"cpp_std", MK(CPPMutex)},
    {"dijkstra", MK(DijkstraMutex)},
    {"dijkstra_nonatomic", MK(DijkstraNonatomicMutex)},
    {"dijkstra_nonatomic_sleeper", MK(DijkstraNonatomicSleeperMutex)},
    {"spin", MK(SpinLock)},
    {"hard_spin", MK(HardSpinLock)},
    {"exp_spin", MK(ExponentialSpinLock)},
    {"wait_spin", MK(WaitSpinLock)},
    {"bakery", MK(BakeryMutex)},
    {"bakery_nonatomic", MK(BakeryNonAtomicMutex)},
    {"lamport", MK(LamportLock)},
    {"lamport_sleeper", MK(LamportSleeperLock)},
    {"mcs", MK(MCSMutex)},
    {"mcs_nca", MK(MCSNonCacheAlignedMutex)},
    {"mcs_local", MK(MCSLocalMutex)},
    {"mcs_sleeper", MK(MCSSleeperMutex)},
    {"mcs_malloc", MK(MCSMallocMutex)},
    {"knuth", MK(KnuthMutex)},
    {"knuth_sleeper", MK(KnuthSleeperMutex)},
    {"peterson", MK(PetersonMutex)},
    {"boulangerie", MK(Boulangerie)},
    {"szymanski", MK(SzymanskiLock)},
    {"ticket", MK(TicketMutex)},
    {"threadlocal_ticket", MK(ThreadlocalTicketMutex)},
    {"ring_ticket", MK(RingTicketMutex)},
    {"null", MK(NullMutex)},
    {"halfnode", MK(HalfnodeMutex)},
    {"hopscotch", MK(HopscotchMutex)},
    {"hopscotch_nca", MK(HopscotchNonCacheAlignedMutex)},
    {"linear_cas_elevator", MK(LinearElevatorMutex<SpinLock>)},
    {"tree_cas_elevator", MK(TreeElevatorMutex<SpinLock>)},
    {"linear_bl_elevator", MK(LinearElevatorMutex<BurnsLamportMutex>)},
    {"tree_bl_elevator", MK(TreeElevatorMutex<BurnsLamportMutex>)},
    {"linear_lamport_elevator", MK(LinearElevatorMutex<LamportLock>)},
    {"tree_lamport_elevator", MK(TreeElevatorMutex<LamportLock>)},
    {"linear_cas_elevator_nca", MK(LinearElevatorNCAMutex<SpinLock>)},
    {"tree_cas_elevator_nca", MK(TreeElevatorNCAMutex<SpinLock>)},
    {"linear_bl_elevator_nca", MK(LinearElevatorNCAMutex<BurnsLamportMutex>)},
    {"tree_bl_elevator_nca", MK(TreeElevatorNCAMutex<BurnsLamportMutex>)},
    {"linear_lamport_elevator_nca", MK(LinearElevatorNCAMutex<LamportLock>)},
    {"tree_lamport_elevator_nca", MK(TreeElevatorNCAMutex<LamportLock>)},
    {"burns_lamport", MK(BurnsLamportMutex)},
    {"net_elevator", MK(NetElevatorMutex)},
    {"yang", MK(YangMutex)},
    {"yang_sleeper", MK(YangSleeperMutex)},
    {"cohortMCS", MK(CMCSLock)},
    {"hbo", MK(hbo_lock)},
    {"cohortTicket", MK(CohortTicket)},
    {"hmcs", MK(hmcs::HMCSLock)},
    {"cohortTAS", MK(CohortTASLock)},
    {"cohortPTicket", MK(CohortPTicketLock)},
    {"hclh", MK(hclh::HCLHMutex)},
    {"bitonic_cas", MK(BitonicCASLock)},
    {"bitonic_bl", MK(BitonicBLLock)},
    {"bitonic_lamport", MK(BitonicLamportLock)},
    {"bitonic_elevator", MK(BitonicElevatorLock)},
    {"bitonic_bakery", MK(BitonicBakeryLock)},
    {"periodic_cas", MK(PeriodicCASLock)},
    {"periodic_bl", MK(PeriodicBLLock)},
    {"periodic_lamport", MK(PeriodicLamportLock)},
    {"periodic_elevator", MK(PeriodicElevatorLock)},
    {"periodic_bakery", MK(PeriodicBakeryLock)},
    {"lw_bitonic_cas", MK(LWBitonicCASLock)},
    {"lw_bitonic_bl", MK(LWBitonicBLLock)},
    {"lw_bitonic_lamport", MK(LWBitonicLamportLock)},
    {"lw_bitonic_bakery", MK(LWBitonicBakeryLock)},
    {"lw_periodic_cas", MK(LWPeriodicCASLock)},
    {"lw_periodic_bl", MK(LWPeriodicBLLock)},
    {"lw_periodic_lamport", MK(LWPeriodicLamportLock)},
    {"lw_periodic_bakery", MK(LWPeriodicBakeryLock)},
    {"seq_bitonic_cas", MK(SeqBitonicCASLock)},
    {"seq_periodic_cas", MK(SeqPeriodicCASLock)},
    {"bo_bitonic_cas", MK(BoBitonicCASLock)},
    {"bo_periodic_cas", MK(BoPeriodicCASLock)},
    {"wf_bitonic_cas", MK(WFBitonicCASLock)},
    {"wf_bitonic_bl", MK(WFBitonicBLLock)},
    {"wf_bitonic_lamport", MK(WFBitonicLamportLock)},
    {"wf_bitonic_bakery", MK(WFBitonicBakeryLock)},
    {"wf_periodic_cas", MK(WFPeriodicCASLock)},
    {"wf_periodic_bl", MK(WFPeriodicBLLock)},
    {"wf_periodic_lamport", MK(WFPeriodicLamportLock)},
    {"wf_periodic_bakery", MK(WFPeriodicBakeryLock)},
    {"skew_bitonic_cas", MK(SkewBitonicCASLock)},
    {"skew_periodic_cas", MK(SkewPeriodicCASLock)},
#ifdef inc_futex
    {"futex", MK(FutexLock)},
#endif
#ifdef inc_boost
    {"boost", MK(BoostMutex)},
#endif
#ifdef inc_nsync
    {"nsync", MK(NSync)},
#endif
#ifdef inc_umwait
    {"umwait", MK(UMWaitLock)},
#endif
};

#undef MK

SoftwareMutex *get_mutex(const char *mutex_name, size_t num_threads) {
    (void)num_threads; // May be used in the future

    auto it = kMutexTable.find(mutex_name);
    if (it == kMutexTable.end()) {
        fprintf(stderr,
            "Unrecognized mutex '%s'\n", mutex_name
        );
        return nullptr;
    }
    return it->second();
}
