#ifndef __BENCH_UTILS_HPP_
#define __BENCH_UTILS_HPP_

#pragma once

#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>
#include <memory>
#include <atomic>
#include <chrono>
#include <vector>
#include <cstdlib>
#include <utility>
#include <unordered_map>


struct per_thread_stats {
    int thread_id;
    int num_iterations;

    std::chrono::nanoseconds run_time;
    struct timespec start_time;
    struct timespec end_time;
    // Vector reallocation could waste some thread time.
    std::vector<double> lock_times;

};


struct run_stats {
    int num_threads;
    struct per_thread_stats **thread_stats;
    struct rusage usage;
};


// Use getrusage  to record resource usage

void record_rusage(bool csv);
void print_rusage(struct rusage *usage, bool csv);

void init_lock_timer(struct per_thread_stats *stats);
void start_lock_timer(struct per_thread_stats *stats);
void end_lock_timer(struct per_thread_stats *stats);
void destroy_lock_timer(struct per_thread_stats *stats);

// void start_timer(struct per_thread_stats *stats);
// void end_timer(struct per_thread_stats *stats);

// void report_thread_stats(struct per_thread_stats *stats, bool csv = false, bool thread_level = true);
void report_run_latency(struct run_stats *stats);

void report_thread_latency(struct per_thread_stats *stats, bool csv, bool thread_level);

struct Node{
    int key, value;
    Node *younger, *older;
};


//optimized LRUCache
class LRUCache {
private:
    std::unordered_map<int, Node*> nodes;
    int capacity;
    Node *birth;
    Node *death;

public:
    LRUCache(int capacity) {
        this->capacity = capacity;
        birth = new Node{-1, -1, nullptr, nullptr};
        death = new Node{-1, -1, nullptr, nullptr};
        birth->older=death;
        death->younger=birth;
    }
    
    int get(int key) {
        if(nodes.find(key)!=nodes.end())
        {
            Node* node = nodes[key];
            node->younger->older=node->older;
            node->older->younger=node->younger;
            node->older=birth->older;
            node->younger=birth;
            birth->older->younger=node;
            birth->older=node;
            return node->value;
        }
        return -1;
    }
    
    void put(int key, int value) {
        if (nodes.find(key)!=nodes.end())
        {
            Node* node = nodes[key];
            node->younger->older=node->older;
            node->older->younger=node->younger;
            node->older=birth->older;
            node->younger=birth;
            birth->older->younger=node;
            birth->older=node;  
            node->value=value;
        }
        else
        {
            if (nodes.size()<capacity)
            {
                Node* node = new Node{key, value, birth, birth->older};
                birth->older->younger=node;
                birth->older=node;
                nodes[key]=node;
            }
            else
            {
                Node* node = new Node{key, value, birth, birth->older};
                nodes.erase(death->younger->key);
                death->younger=death->younger->younger;
                death->younger->older=death;                
                birth->older->younger=node;
                birth->older=node;
                nodes[key]=node;
            }
        }
    }

    std::pair<std::vector<int>, std::vector<int>> add_pairs(int n){
        std::vector<int> keys;
        std::vector<int> values;
        for (int i=0; i<n; i++){
            keys.push_back(rand());
            values.push_back(rand());
            put(keys[i], values[i]);
        }

        return std::make_pair(keys, values);
    }
};
#endif // __BENCH_UTILS_HPP_