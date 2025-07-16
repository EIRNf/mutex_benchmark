#include "bakery_static_mutex.c"
#include "emucxl/src/emucxl_lib.h"

// #include <threads.h>
#include <bits/pthreadtypes.h>
#include <pthread.h>

#define NUM_THREADS 10
#define NUM_ITERATIONS 1000

#define LOCAL_MEMORY 0
#define REMOTE_CXL_MEMORY 1

struct thread_info {
    size_t thread_id;
    struct bakery_static_mutex *mutex;
    size_t *counter;
    bool *start_flag;
};

static void *thread_start(void *arg)
{
    struct thread_info *thread_info = (struct thread_info*)arg;
    while (!thread_info->start_flag);

    for (size_t i = 0; i < NUM_ITERATIONS; i++) {
        bakery_static_mutex_lock(thread_info->mutex, thread_info->thread_id);
        thread_info->counter++;
        bakery_static_mutex_unlock(thread_info->mutex, thread_info->thread_id);
    }
}

void test_bakery()
{
    size_t region_size = bakery_static_mutex_get_size(NUM_THREADS);
    void *region = emucxl_alloc(region_size, REMOTE_CXL_MEMORY);
    struct bakery_static_mutex *mutex = bakery_static_mutex_init(region, NUM_THREADS);
    // Should these be in shared memory?
    size_t counter;
    bool start_flag = false;
    pthread_t threads[NUM_THREADS];
    struct thread_info threads_info[NUM_THREADS];
    
    for (size_t thread_id = 0; thread_id < NUM_THREADS; thread_id++)
    {
        struct thread_info info = { thread_id, mutex, &counter, &start_flag };
        threads_info[thread_id] = info;
        pthread_create(&threads[thread_id], NULL, &thread_start, &threads_info[thread_id]);
    }

    start_flag = true;

    void *_ret;
    for (size_t thread_id = 0; thread_id < NUM_THREADS; thread_id++)
    {
        pthread_join(threads[thread_id], &_ret);
    }

    emucxl_free(region, region_size);
}

int main(int argc, char **argv)
{
    emucxl_init();

    printf("Testing bakery... ");
    test_bakery();
    printf("done\n");

    emucxl_exit();

    printf("Done testing.\n");
    return 0;
}