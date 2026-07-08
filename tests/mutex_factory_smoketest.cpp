// Smoke test for get_mutex(): constructs every mutex name that should be
// reachable in the current build configuration, exercises the full
// init/lock/unlock/destroy lifecycle once (single-threaded, no contention),
// and reports each returned name() string. Used as a regression oracle when
// refactoring get_mutex()'s factory chain in lib/utils/bench_utils.cpp.

#include "bench_utils.hpp"
#include <cstdio>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>

// Names guaranteed present regardless of optional inc_futex/inc_boost/
// inc_nsync/inc_umwait build flags (those four are tested separately,
// gated the same way, since they require extra libraries to link).
static const char *kDefaultNames[] = {
    "bakery",
    "bakery_nonatomic",
    "bitonic_bakery",
    "bitonic_bl",
    "bitonic_cas",
    "bitonic_elevator",
    "bitonic_lamport",
    "boulangerie",
    "broken",
    "burns_lamport",
    "clh",
    "cohortMCS",
    "cohortPTicket",
    "cohortTAS",
    "cohortTicket",
    "cpp_std",
    "dijkstra",
    "dijkstra_nonatomic",
    "dijkstra_nonatomic_sleeper",
    "elevator",
    "exp_spin",
    "halfnode",
    "hard_spin",
    "hbo",
    "hclh",
    "hmcs",
    "hopscotch",
    "hopscotch_local",
    "hopscotch_nca",
    "knuth",
    "knuth_sleeper",
    "lamport",
    "lamport_sleeper",
    "linear_bl_elevator",
    "linear_bl_elevator_nca",
    "linear_cas_elevator",
    "linear_cas_elevator_nca",
    "linear_lamport_elevator",
    "linear_lamport_elevator_nca",
    "lw_bitonic_bakery",
    "lw_bitonic_bl",
    "lw_bitonic_cas",
    "lw_bitonic_lamport",
    "lw_periodic_bakery",
    "lw_periodic_bl",
    "lw_periodic_cas",
    "lw_periodic_lamport",
    "mcs",
    "mcs_local",
    "mcs_malloc",
    "mcs_nca",
    "mcs_sleeper",
    "net_elevator",
    "null",
    "periodic_bakery",
    "periodic_bl",
    "periodic_cas",
    "periodic_elevator",
    "periodic_lamport",
    "peterson",
    "ring_ticket",
    "rskew_bitonic_bakery",
    "rskew_bitonic_bl",
    "rskew_bitonic_cas",
    "rskew_bitonic_lamport",
    "rskew_periodic_bakery",
    "rskew_periodic_bl",
    "rskew_periodic_cas",
    "rskew_periodic_lamport",
    "seq_bitonic_bakery",
    "seq_bitonic_bl",
    "seq_bitonic_cas",
    "seq_bitonic_lamport",
    "seq_periodic_bakery",
    "seq_periodic_bl",
    "seq_periodic_cas",
    "seq_periodic_lamport",
    "skew_bitonic_bakery",
    "skew_bitonic_bl",
    "skew_bitonic_cas",
    "skew_bitonic_lamport",
    "skew_periodic_bakery",
    "skew_periodic_bl",
    "skew_periodic_cas",
    "skew_periodic_lamport",
    "spin",
    "system",
    "szymanski",
    "threadlocal_ticket",
    "ticket",
    "tree_bl_elevator",
    "tree_bl_elevator_nca",
    "tree_cas_elevator",
    "tree_cas_elevator_nca",
    "tree_lamport_elevator",
    "tree_lamport_elevator_nca",
    "wait_spin",
    "wf_bitonic_bakery",
    "wf_bitonic_bl",
    "wf_bitonic_cas",
    "wf_bitonic_lamport",
    "wf_periodic_bakery",
    "wf_periodic_bl",
    "wf_periodic_cas",
    "wf_periodic_lamport",
    "yang",
    "yang_sleeper",
};

// Only reachable when the build was configured with the matching
// -Dinc_<name> flag (see scripts/builder.py CONDITIONAL_COMPILATION_MUTEXES).
static const char *kConditionalNames[] = {
#ifdef inc_futex
    "futex",
#endif
#ifdef inc_boost
    "boost",
#endif
#ifdef inc_nsync
    "nsync",
#endif
#ifdef inc_umwait
    "umwait",
#endif
};

// Runs the actual lock exercise in-process (used inside the forked child).
static int exercise_one_unsafe(const char *mutex_name) {
    SoftwareMutex *lock = get_mutex(mutex_name, 4);
    if (lock == nullptr) {
        printf("FAIL  %-30s get_mutex() returned nullptr\n", mutex_name);
        return 1;
    }

    std::string reported_name = lock->name();
    lock->init(4);
    lock->lock(0);
    lock->unlock(0);
    lock->destroy();
    delete lock;

    printf("OK    %-30s name()=\"%s\"\n", mutex_name, reported_name.c_str());
    return 0;
}

// Some lock implementations crash (SIGBUS/SIGSEGV) on this machine's
// architecture even in the uncontended single-thread case (pre-existing bugs,
// not introduced by this test). Fork per-name so one crashing lock doesn't
// prevent the rest of the baseline from being captured.
static int exercise_one(const char *mutex_name) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        // child: _exit() skips stdio flushing, so flush explicitly before
        // exiting (and before any possible crash discards the buffer).
        int result = exercise_one_unsafe(mutex_name);
        fflush(stdout);
        _exit(result);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) {
        printf("CRASH %-30s terminated by signal %d\n", mutex_name, WTERMSIG(status));
        return 1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

int main() {
    int failures = 0;
    int total = 0;

    for (const char *name : kDefaultNames) {
        total++;
        failures += exercise_one(name);
    }
    for (const char *name : kConditionalNames) {
        total++;
        failures += exercise_one(name);
    }

    // Also confirm unknown names are still rejected the same way.
    total++;
    if (get_mutex("not_a_real_mutex", 4) != nullptr) {
        printf("FAIL  %-30s expected nullptr for unknown name\n", "not_a_real_mutex");
        failures++;
    } else {
        printf("OK    %-30s correctly rejected\n", "not_a_real_mutex");
    }

    printf("\n%d/%d checks passed, %d failed\n", total - failures, total, failures);
    return failures == 0 ? 0 : 1;
}
