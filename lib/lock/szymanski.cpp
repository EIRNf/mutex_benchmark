#include "lock.hpp"
#include <atomic>
#include <vector>
#include <thread>
#include <string>

class SzymanskiLock : public virtual SoftwareMutex {
public:
    void init(size_t n_threads) override {
        num_threads = n_threads;
        flags = std::vector<std::atomic<int>>(num_threads);
        for (size_t i = 0; i < num_threads; ++i) {
            flags[i].store(0, std::memory_order_relaxed);
        }
    }

    void lock(size_t id) override {
        flags[id].store(1, std::memory_order_seq_cst);

        for (size_t j = 0; j < num_threads; ++j) {
            if (j == id) continue;
            while (flags[j].load(std::memory_order_seq_cst) >= 3) {
                std::this_thread::yield();
            }
        }

        flags[id].store(3, std::memory_order_seq_cst);

        bool waiting = false;
        for (size_t j = 0; j < num_threads; ++j) {
            if (j == id) continue;
            if (flags[j].load(std::memory_order_seq_cst) == 1) {
                waiting = true;
                break;
            }
        }

        if (waiting) {
            flags[id].store(2, std::memory_order_seq_cst);

            bool someone_in_cs = false;
            while (!someone_in_cs) {
                for (size_t j = 0; j < num_threads; ++j) {
                    if (flags[j].load(std::memory_order_seq_cst) == 4) {
                        someone_in_cs = true;
                        break;
                    }
                }
                if (!someone_in_cs) std::this_thread::yield();
            }

            flags[id].store(3, std::memory_order_seq_cst);
        }

        flags[id].store(4, std::memory_order_seq_cst);

        for (size_t j = 0; j < id; ++j) {
            while (flags[j].load(std::memory_order_seq_cst) >= 2) {
                std::this_thread::yield();
            }
        }
    }

    void unlock(size_t id) override {
        for (size_t j = 0; j < num_threads; ++j) {
            if (j <= id) continue;
            while (true) {
                int val = flags[j].load(std::memory_order_seq_cst);
                if (val == 0 || val == 1 || val == 4) break;
                std::this_thread::yield();
            }
        }

        flags[id].store(0, std::memory_order_seq_cst);
    }

    std::string name() override {
        return "szymanski";
    }

    void destroy() override {
        flags.clear();
    }

private:
    size_t num_threads;
    std::vector<std::atomic<int>> flags;
};
