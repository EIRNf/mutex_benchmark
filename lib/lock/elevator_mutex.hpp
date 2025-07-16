#ifndef LOCK_ELEVATOR_HPP
#define LOCK_ELEVATOR_HPP

#include "lock.hpp"
#include <stdexcept>
#include <atomic>
#include <thread>
#include <vector>
#include <string>

#include <cassert>

#pragma once

class ElevatorMutex : public SoftwareMutex {
public:
    struct Node {
        std::atomic<Node*> next{nullptr};
        std::atomic<bool> waiting{false};
    };

private:
    std::atomic<Node*> tail{nullptr};
    std::vector<Node*> nodes;
    size_t max_threads = 0;

public:
    ~ElevatorMutex() override { destroy(); }

    void init(size_t num_threads) override {
        destroy();
        max_threads = num_threads;
        nodes.resize(num_threads);
        for (size_t i = 0; i < num_threads; ++i) {
            nodes[i] = new Node{};
        }
        tail.store(nullptr);
    }

    void lock(size_t thread_id) override {
        // printf("%ld: Locking...\n", thread_id);
        assert(thread_id < max_threads);
        Node* me = nodes[thread_id];
        me->next.store(nullptr);
        me->waiting.store(true);

        Node* prev = tail.exchange(me);
        if (prev != nullptr) {
            prev->next.store(me);
            while (me->waiting.load()) {
                std::this_thread::yield();
            }
        }
        // if prev == nullptr, lock is immediately given 
        // printf("%ld: Locked\n", thread_id);
    }

    void unlock(size_t thread_id) override {
        // printf("%ld: Locked\n", thread_id);
        assert(thread_id < max_threads);
        Node* me = nodes[thread_id];
        Node* succ = me->next.load();

        if (succ == nullptr) {
            // no known successor: try resetting tail
            if (tail.compare_exchange_strong(me, nullptr)) {
                // printf("%ld: Unlocked\n", thread_id);
                return; // lock released, no waiters
            }

            // wait for successor to show up
            do {
                succ = me->next.load();
                // std::this_thread::yield();
            } while (succ == nullptr);
        }
 
        succ->waiting.store(false);
        // printf("%ld: Unlocked\n", thread_id);
    }

    void destroy() override {
        for (auto p : nodes) {
            delete p;
        }
        nodes.clear();
        max_threads = 0;
        tail.store(nullptr);
    }

    std::string name() override {
        return "ElevatorMutex (circular list + CAS)";
    }
};

#endif // LOCK_ELEVATOR_HPP