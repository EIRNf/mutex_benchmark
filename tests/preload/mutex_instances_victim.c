/* Standalone LD_PRELOAD victim for multi-instance mutex checks.
 * Build with gcc -std=c11 -pthread and run as mutex_instances_victim
 * [independent|nested|both], setting MUTEX_SHIM_LOCK in the environment.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define INDEPENDENT_ITERS 120
#define NESTED_ITERS 80
#define NESTED_B_WARMUP 7
#define NESTED_THREADS 8

static pthread_mutex_t locks[2] = {
    PTHREAD_MUTEX_INITIALIZER,
    PTHREAD_MUTEX_INITIALIZER,
};

static _Atomic int inside[2];
static _Atomic int total_inside;
static _Atomic int max_total_inside;
static _Atomic int violations;
static int counts[2];
static _Atomic int nested_count;

struct worker_arg {
    int lock_index;
};

static void record_max_total(int total) {
    int current = atomic_load(&max_total_inside);
    while (current < total &&
           !atomic_compare_exchange_weak(&max_total_inside, &current, total)) {
    }
}

static void *independent_worker(void *arg) {
    int index = ((struct worker_arg *)arg)->lock_index;
    const struct timespec delay = {0, 100000};

    for (int i = 0; i < INDEPENDENT_ITERS; ++i) {
        if (pthread_mutex_lock(&locks[index]) != 0) {
            atomic_fetch_add(&violations, 1);
            return NULL;
        }

        if (atomic_fetch_add(&inside[index], 1) != 0) {
            atomic_fetch_add(&violations, 1);
        }
        int total = atomic_fetch_add(&total_inside, 1) + 1;
        record_max_total(total);
        counts[index]++;
        nanosleep(&delay, NULL);
        atomic_fetch_sub(&total_inside, 1);
        atomic_fetch_sub(&inside[index], 1);

        if (pthread_mutex_unlock(&locks[index]) != 0) {
            atomic_fetch_add(&violations, 1);
            return NULL;
        }
    }
    return NULL;
}

static void *nested_worker(void *arg) {
    (void)arg;
    const struct timespec delay = {0, 100000};
    for (int i = 0; i < NESTED_ITERS; ++i) {
        if (pthread_mutex_lock(&locks[0]) != 0) {
            atomic_fetch_add(&violations, 1);
            return NULL;
        }
        /* Let same-cohort waiters queue on A before the thread also uses B. */
        nanosleep(&delay, NULL);
        if (pthread_mutex_lock(&locks[1]) != 0) {
            atomic_fetch_add(&violations, 1);
            pthread_mutex_unlock(&locks[0]);
            return NULL;
        }

        atomic_fetch_add(&nested_count, 1);

        if (pthread_mutex_unlock(&locks[1]) != 0) {
            atomic_fetch_add(&violations, 1);
        }
        if (pthread_mutex_unlock(&locks[0]) != 0) {
            atomic_fetch_add(&violations, 1);
        }
    }
    return NULL;
}

static int run_independent(void) {
    pthread_t threads[4];
    struct worker_arg args[4];

    fprintf(stderr, "PHASE independent starting\n");
    for (int i = 0; i < 4; ++i) {
        args[i].lock_index = i % 2;
        int rc = pthread_create(&threads[i], NULL, independent_worker, &args[i]);
        if (rc != 0) {
            fprintf(stderr, "pthread_create failed: %d\n", rc);
            return 1;
        }
    }
    for (int i = 0; i < 4; ++i) {
        int rc = pthread_join(threads[i], NULL);
        if (rc != 0) {
            fprintf(stderr, "pthread_join failed: %d\n", rc);
            return 1;
        }
    }

    printf("INDEPENDENT overlap=%d violations=%d counts=%d,%d\n",
           atomic_load(&max_total_inside), atomic_load(&violations),
           counts[0], counts[1]);
    fflush(stdout);
    return atomic_load(&violations) != 0 ||
           atomic_load(&max_total_inside) < 2 ||
           counts[0] != 2 * INDEPENDENT_ITERS ||
           counts[1] != 2 * INDEPENDENT_ITERS;
}

static int run_nested(void) {
    pthread_t threads[NESTED_THREADS];

    fprintf(stderr, "PHASE nested starting\n");
    /* Offset B's queue state so shared per-thread ticket state is observable. */
    for (int i = 0; i < NESTED_B_WARMUP; ++i) {
        if (pthread_mutex_lock(&locks[1]) != 0 ||
            pthread_mutex_unlock(&locks[1]) != 0) {
            fprintf(stderr, "nested warm-up failed at iteration %d\n", i);
            return 1;
        }
    }
    for (int i = 0; i < NESTED_THREADS; ++i) {
        int rc = pthread_create(&threads[i], NULL, nested_worker, NULL);
        if (rc != 0) {
            fprintf(stderr, "pthread_create failed: %d\n", rc);
            return 1;
        }
    }
    for (int i = 0; i < NESTED_THREADS; ++i) {
        int rc = pthread_join(threads[i], NULL);
        if (rc != 0) {
            fprintf(stderr, "pthread_join failed: %d\n", rc);
            return 1;
        }
    }

    printf("NESTED completed=%d violations=%d\n",
           atomic_load(&nested_count), atomic_load(&violations));
    fflush(stdout);
    return atomic_load(&violations) != 0 ||
           atomic_load(&nested_count) != NESTED_THREADS * NESTED_ITERS;
}

int main(int argc, char **argv) {
    const char *phase = argc > 1 ? argv[1] : "both";
    int rc = 0;

    if (strcmp(phase, "independent") == 0) {
        rc = run_independent();
    } else if (strcmp(phase, "nested") == 0) {
        rc = run_nested();
    } else if (strcmp(phase, "both") == 0) {
        rc = run_independent();
        if (rc == 0) {
            rc = run_nested();
        }
    } else {
        fprintf(stderr, "usage: %s [independent|nested|both]\n", argv[0]);
        return 2;
    }

    int destroy_rc = pthread_mutex_destroy(&locks[0]);
    int second_destroy_rc = pthread_mutex_destroy(&locks[1]);
    if (destroy_rc != 0 || second_destroy_rc != 0) {
        fprintf(stderr, "pthread_mutex_destroy failed: %d,%d\n",
                destroy_rc, second_destroy_rc);
        rc = 1;
    }
    return rc;
}
