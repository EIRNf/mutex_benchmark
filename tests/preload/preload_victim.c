/* preload_victim.c — pure-POSIX pthread application used to smoke-test the
 * preload shim. It deliberately exercises every interception path the shim
 * supports, and knows nothing about the benchmark's headers:
 *
 *   - statically initialized mutex (PTHREAD_MUTEX_INITIALIZER, lazy claim)
 *   - heap mutex initialized via pthread_mutex_init
 *   - attr-recursive mutex, locked 3 deep (shim recursion emulation)
 *   - trylock spin loop
 *   - condvar producer/consumer queue (shadow-mutex protocol, lost-wakeup
 *     check: the consumer only ever wakes via cond signals)
 *   - sequential thread churn beyond MUTEX_SHIM_MAX_THREADS (id recycling)
 *   - destroy paths for the dynamic mutexes and the condvar
 *
 * Exits 0 iff every counter matches its expected value, so it doubles as a
 * mutual-exclusion checker for the injected lock. Usage:
 *   preload_victim [nthreads] [iters]
 */
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHURN_THREADS 96

static pthread_mutex_t static_mtx = PTHREAD_MUTEX_INITIALIZER;
static long c_static;

static pthread_mutex_t *dyn_mtx;
static long c_dyn;

static pthread_mutex_t rec_mtx;
static long c_rec;

static pthread_mutex_t try_mtx = PTHREAD_MUTEX_INITIALIZER;
static long c_try;
static long c_try_busy;

static pthread_mutex_t q_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_cv = PTHREAD_COND_INITIALIZER;
static long q_pending;

#define PP_ROUNDS 2000
static pthread_mutex_t pp_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pp_cv = PTHREAD_COND_INITIALIZER;
static int pp_turn;      /* 0: ping's turn, 1: pong's turn */
static long c_pp;

#define GATE_WAITERS 3
static pthread_mutex_t gate_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
static int gate_open;
static long c_gate;

static int n_threads = 4;
static long iters = 20000;

static void *worker(void *arg) {
    (void)arg;
    long busy = 0;
    for (long i = 0; i < iters; i++) {
        pthread_mutex_lock(&static_mtx);
        c_static++;
        pthread_mutex_unlock(&static_mtx);
    }
    for (long i = 0; i < iters; i++) {
        pthread_mutex_lock(dyn_mtx);
        c_dyn++;
        pthread_mutex_unlock(dyn_mtx);
    }
    for (long i = 0; i < iters; i++) {
        pthread_mutex_lock(&rec_mtx);
        pthread_mutex_lock(&rec_mtx);
        pthread_mutex_lock(&rec_mtx);
        c_rec++;
        pthread_mutex_unlock(&rec_mtx);
        pthread_mutex_unlock(&rec_mtx);
        pthread_mutex_unlock(&rec_mtx);
    }
    for (long i = 0; i < iters; i++) {
        while (pthread_mutex_trylock(&try_mtx) != 0) {
            busy++;
            sched_yield();
        }
        c_try++;
        pthread_mutex_unlock(&try_mtx);
    }
    pthread_mutex_lock(&static_mtx);
    c_try_busy += busy;
    pthread_mutex_unlock(&static_mtx);
    return NULL;
}

static void *producer(void *arg) {
    (void)arg;
    for (long i = 0; i < iters; i++) {
        pthread_mutex_lock(&q_mtx);
        q_pending++;
        pthread_cond_signal(&q_cv);
        pthread_mutex_unlock(&q_mtx);
    }
    return NULL;
}

static void *consumer(void *arg) {
    long target = *(long *)arg;
    for (long got = 0; got < target; got++) {
        pthread_mutex_lock(&q_mtx);
        while (q_pending == 0) {
            pthread_cond_wait(&q_cv, &q_mtx);
        }
        q_pending--;
        pthread_mutex_unlock(&q_mtx);
    }
    return NULL;
}

/* Strict alternation: each side MUST block waiting for the other's turn, so
 * this phase deterministically drives pthread_cond_wait through the shim
 * (unlike the queue, where fast producers may keep the consumer from ever
 * blocking). A lost wakeup here hangs the test. */
static void *pingpong(void *arg) {
    int my_turn = (int)(intptr_t)arg;
    for (int i = 0; i < PP_ROUNDS; i++) {
        pthread_mutex_lock(&pp_mtx);
        while (pp_turn != my_turn) {
            pthread_cond_wait(&pp_cv, &pp_mtx);
        }
        c_pp++;
        pp_turn = 1 - my_turn;
        pthread_cond_signal(&pp_cv);
        pthread_mutex_unlock(&pp_mtx);
    }
    return NULL;
}

/* Gate: waiters arrive long before main opens the gate (main sleeps first),
 * so at least GATE_WAITERS pthread_cond_wait calls are guaranteed — the
 * ping-pong and queue phases can, with lucky scheduling, run without any
 * thread ever parking. Also exercises pthread_cond_broadcast. */
static void *gate_waiter(void *arg) {
    (void)arg;
    pthread_mutex_lock(&gate_mtx);
    while (!gate_open) {
        pthread_cond_wait(&gate_cv, &gate_mtx);
    }
    c_gate++;
    pthread_mutex_unlock(&gate_mtx);
    return NULL;
}

static void *churn_worker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&static_mtx);
    c_static++;
    pthread_mutex_unlock(&static_mtx);
    return NULL;
}

static int check(const char *name, long got, long want) {
    if (got == want) return 0;
    fprintf(stderr, "VICTIM FAIL: %s = %ld, expected %ld\n", name, got, want);
    return 1;
}

int main(int argc, char **argv) {
    if (argc > 1) n_threads = atoi(argv[1]);
    if (argc > 2) iters = atol(argv[2]);
    if (n_threads < 1 || iters < 1) {
        fprintf(stderr, "usage: %s [nthreads] [iters]\n", argv[0]);
        return 2;
    }

    dyn_mtx = malloc(sizeof *dyn_mtx);
    pthread_mutex_init(dyn_mtx, NULL);

    pthread_mutexattr_t rattr;
    pthread_mutexattr_init(&rattr);
    pthread_mutexattr_settype(&rattr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&rec_mtx, &rattr);
    pthread_mutexattr_destroy(&rattr);

    long q_target = (long)n_threads * iters;

    pthread_t *ts = malloc(sizeof(pthread_t) * (size_t)(2 * n_threads + 1));
    int nt = 0;
    for (int i = 0; i < n_threads; i++) {
        pthread_create(&ts[nt++], NULL, worker, NULL);
    }
    for (int i = 0; i < n_threads; i++) {
        pthread_create(&ts[nt++], NULL, producer, NULL);
    }
    pthread_create(&ts[nt++], NULL, consumer, &q_target);
    for (int i = 0; i < nt; i++) {
        pthread_join(ts[i], NULL);
    }

    pthread_t ping, pong;
    pthread_create(&ping, NULL, pingpong, (void *)(intptr_t)0);
    pthread_create(&pong, NULL, pingpong, (void *)(intptr_t)1);
    pthread_join(ping, NULL);
    pthread_join(pong, NULL);

    pthread_t gw[GATE_WAITERS];
    for (int i = 0; i < GATE_WAITERS; i++) {
        pthread_create(&gw[i], NULL, gate_waiter, NULL);
    }
    struct timespec nap = {0, 150 * 1000 * 1000};
    nanosleep(&nap, NULL);
    pthread_mutex_lock(&gate_mtx);
    gate_open = 1;
    pthread_cond_broadcast(&gate_cv);
    pthread_mutex_unlock(&gate_mtx);
    for (int i = 0; i < GATE_WAITERS; i++) {
        pthread_join(gw[i], NULL);
    }

    /* Sequential churn: total thread count far exceeds a small
     * MUTEX_SHIM_MAX_THREADS, but concurrency stays low — passes only if the
     * shim recycles dense thread ids at thread exit. */
    for (int i = 0; i < CHURN_THREADS; i++) {
        pthread_t t;
        pthread_create(&t, NULL, churn_worker, NULL);
        pthread_join(t, NULL);
    }

    pthread_mutex_destroy(dyn_mtx);
    free(dyn_mtx);
    pthread_mutex_destroy(&rec_mtx);
    pthread_cond_destroy(&q_cv);

    long per = (long)n_threads * iters;
    int bad = 0;
    bad += check("static counter", c_static, per + CHURN_THREADS);
    bad += check("dynamic counter", c_dyn, per);
    bad += check("recursive counter", c_rec, per);
    bad += check("trylock counter", c_try, per);
    bad += check("queue drained", q_pending, 0);
    bad += check("pingpong rounds", c_pp, 2L * PP_ROUNDS);
    bad += check("gate waiters", c_gate, GATE_WAITERS);
    if (bad) {
        return 1;
    }
    printf("VICTIM OK: threads=%d iters=%ld counters=%ld churn=%d trylock_busy=%ld\n",
           n_threads, iters, per, CHURN_THREADS, c_try_busy);
    return 0;
}
