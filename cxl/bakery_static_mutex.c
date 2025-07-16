#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <threads.h>
#include <string.h>
#include <stdatomic.h>

// TODO add fences because these variables are not going to be atomic in CXL
// TODO handle emucxl_write() returning false
// Done: restructure -- remove unnecessary pointers. following mutex->number
// almost certainly takes longer than the equivalent addition & multiplication.
// Note: restructuring absolutely needed because mutex->choosing doesn't work -- it needs emucxl_write instead.

// TODO: does this code get ruined by endianness? the buffer stuff seems like it might be.
// TODO: storing num_threads locally as part of the pointer (fat pointer) would be better; it doesn't change.

// These have to be statics so that we can get pointers to them
// Should be constant, but the API doesn't allow it.
// static bool true_ = true;
// static bool false_ = false;
// static size_t not_in_contention = 0;

struct bakery_static_mutex {
    size_t num_threads;
    char tail[];
};

bool *bm_get_choosing_array(struct bakery_static_mutex *mutex, size_t num_threads) {
    return (bool*)&mutex->tail[sizeof(size_t) * num_threads];
}

size_t *bm_get_number_array(struct bakery_static_mutex *mutex, size_t num_threads) {
    return (size_t*)&mutex->tail[0];
}

size_t bakery_static_mutex_get_size(size_t num_threads)
{
    // This would break if sizeof(bool) were less than number_size and caused it to be misaligned
    size_t number_size = sizeof(size_t) * num_threads;
    size_t choosing_size = sizeof(bool) * num_threads;
    return sizeof(struct bakery_static_mutex) + number_size + choosing_size;
}

struct bakery_static_mutex *bakery_static_mutex_init(void *region, size_t num_threads)
{
    struct bakery_static_mutex *mutex = (struct bakery_static_mutex*)region;
    emucxl_write(&mutex->num_threads, 0, &num_threads, sizeof(mutex->num_threads));
    memset((void*)&mutex->tail, 0, num_threads * (sizeof(size_t) + sizeof(bool)));
}

void bakery_static_mutex_lock(struct bakery_static_mutex *mutex, size_t thread_id) 
{
    // Get "bakery number"
    size_t num_threads = mutex->num_threads;
    bool *choosing = bm_get_choosing_array(mutex, num_threads);
    size_t *number = bm_get_number_array(mutex, num_threads);

    choosing[thread_id] = true;
    atomic_thread_fence(memory_order_seq_cst);
    size_t my_bakery_number = 1;
    for (size_t i = 0; i < num_threads; i++) {
        size_t other_number = number[i];
        if (other_number + 1 > my_bakery_number) {
            my_bakery_number = other_number + 1;
        }
    }
    number[thread_id] = my_bakery_number;

    atomic_thread_fence(memory_order_seq_cst);
    choosing[thread_id] = false;
    atomic_thread_fence(memory_order_seq_cst);

    // Lock waiting part
    for (size_t j = 0; j < num_threads; j++) {
        // Wait until thread j is finished choosing its number if it's just now writing.
        bool other_thread_choosing;
        do {
            other_thread_choosing = choosing[j];
        }
        while (other_thread_choosing != 0);

        // Wait until our number is less than thread[j]'s.
        size_t other_number;
        do {
            other_number = number[j];
        } while ((other_number != 0 && other_number < my_bakery_number) 
            || (other_number == my_bakery_number && j < thread_id));
    }
    
    atomic_thread_fence(memory_order_seq_cst);
}

void bakery_static_mutex_unlock(struct bakery_static_mutex *mutex, size_t thread_id)
{
    atomic_thread_fence(memory_order_seq_cst);
    size_t *number = bm_get_number_array(mutex, mutex->num_threads);
    number[thread_id] = 0;
}