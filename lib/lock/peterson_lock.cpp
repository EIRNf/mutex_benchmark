#include "lock.hpp"
#include "../utils/cxl_utils.hpp"
#include "../utils/region_layout.hpp"
#include <stdexcept>
#include <atomic>

class PetersonMutex : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        // Previously two independent ALLOCATE calls -- under -Dhardware_cxl
        // each is its own NUMA placement decision, so `level` and
        // `last_to_enter` weren't guaranteed to land on the same node even
        // though they're accessed together on every lock()/unlock(). Now
        // carved out of a single region like the other locks in this file.
        RegionLayout layout;
        auto level_handle = layout.reserve_array<std::atomic<int>>(num_threads);
        auto last_to_enter_handle = layout.reserve_array<std::atomic<size_t>>(num_threads);

        _cxl_region_size = layout.total_size();
        _cxl_region = (volatile char*)ALLOCATE(_cxl_region_size);
        this->level         = RegionLayout::resolve(level_handle, _cxl_region);
        this->last_to_enter = RegionLayout::resolve(last_to_enter_handle, _cxl_region); // Does not need initialization.

        for (size_t i = 0; i < num_threads; i++) {
            this->level[i] = -1;
        }
        this->num_threads = num_threads;
    }

    void lock(size_t thread_id) override {
        for (size_t my_level = 0; my_level < num_threads - 1; my_level++) {
            // printf("%ld: my_level=%ld\n", thread_id, my_level);
            level[thread_id] = my_level;
            last_to_enter[my_level] = thread_id;
            while (true) {
                if (last_to_enter[my_level] != thread_id) {
                    goto next_level;
                }
                bool other_thread_higher = false;
                for (size_t other_thread_id = 0; other_thread_id < num_threads; other_thread_id++) {
                    if (other_thread_id != thread_id && level[other_thread_id] >= level[thread_id]) {
                        other_thread_higher = true;
                    }
                }
                if (!other_thread_higher) {
                    goto next_level;
                }
            }
        next_level:;
        }
        // printf("%ld: Locked.\n", thread_id);
    }

    void unlock(size_t thread_id) override {
        level[thread_id] = -1;
        // printf("%ld: Unlocked.\n", thread_id);
    }

    void destroy() override {
        FREE((void*)_cxl_region, _cxl_region_size);
    }

    std::string name() override {
        return "peterson";
    }

private:
    volatile char *_cxl_region;
    size_t _cxl_region_size;
    // Could use something smaller than size_t here
    volatile std::atomic<int> *level;
    volatile std::atomic<size_t> *last_to_enter;
    size_t num_threads;
};
