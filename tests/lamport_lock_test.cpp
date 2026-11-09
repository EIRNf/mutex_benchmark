#include "bench_utils.hpp"

#include <atomic>
#include <cassert>
#include <csignal>
#include <thread>
#include <unistd.h>
#include <vector>

int main() {
    std::signal(SIGALRM, [](int) { _exit(1); });
    alarm(5);

    SoftwareMutex *lock = get_mutex("lamport", 4);
    assert(lock != nullptr);
    lock->init(4);

    std::atomic<bool> start{false};
    std::atomic<unsigned> ready{0};
    std::atomic<unsigned> completed{0};
    std::vector<std::thread> workers;
    for (unsigned id = 0; id < 4; ++id) {
        workers.emplace_back([&, id] {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            lock->lock(id);
            completed.fetch_add(1, std::memory_order_relaxed);
            lock->unlock(id);
        });
    }

    while (ready.load(std::memory_order_acquire) != workers.size()) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    for (auto &worker : workers) {
        worker.join();
    }

    alarm(0);
    assert(completed.load(std::memory_order_relaxed) == workers.size());
    lock->destroy();
    delete lock;
}
