#include "grouped_contention_bench.hpp"
#include "bench_utils.hpp"

#include "lock.hpp"
#include "pthread_lock.cpp"
#include "cpp_std_mutex.cpp"
#include "boost_lock.cpp"
#include "dijkstra_lock.cpp"
#include "spin_lock.cpp"
#include "nsync_lock.cpp"
#include "exp_spin_lock.cpp"
#include "bakery_mutex.cpp"

#include <iostream>

int grouped_contention_bench(int num_threads, std::chrono::nanoseconds run_time, int num_groups, bool csv, SoftwareMutex* lock) {

    // Create run args structure to hold thread arguments
    // struct run_args args;
    // args.num_threads = num_threads;
    // args.thread_args = new per_thread_args*[num_threads];

    // Create shared memory for the lock
    // This could be a simple pointer or a more complex shared memory structure
    // void* shared_memory = nullptr; // Replace with actual shared memory allocation if needed

    // Initialize the lock
    lock->init(num_threads);

    // Create a flag to signal the threads to start
    std::shared_ptr<std::atomic<bool>*> start_flags = std::make_shared<std::atomic<bool>*>((std::atomic<bool>*) malloc(sizeof(std::atomic<bool>*) * num_groups));
    std::shared_ptr<std::atomic<bool>*> end_flags = std::make_shared<std::atomic<bool>*>((std::atomic<bool>*) malloc(sizeof(std::atomic<bool>*) * num_groups));
    volatile int *counter = (volatile int*)malloc(sizeof(int));

    for (int i=0; i<num_groups; i++){
        (*start_flags)[i]=false;
        (*end_flags)[i]=false;
    }

    // Create an array of thread arguments
    std::vector<per_thread_args> thread_args(num_threads);
    for (int i = 0; i < num_threads; ++i) {
        thread_args[i].thread_id = i;
        thread_args[i].lock = lock; // Pass the lock to each thread
        thread_args[i].stats.run_time=run_time;
    }

    // Create an array of threads
    std::vector<std::thread> threads(num_threads);
    for (int i = 0; i < num_threads; ++i) {
        thread_args[i].start_flags = start_flags; // Share the start flag with each thread
        thread_args[i].end_flags = end_flags;
        threads[i] = std::thread([&thread_args, i, counter, &num_groups, &num_threads]() {

                // Record the thread ID
                thread_args[i].stats.thread_id = thread_args[i].thread_id;
            
                // Each thread will run this function

                int group_num = num_groups*(((double)thread_args[i].thread_id)/((double)num_threads));
                // std::cout<<group_num<<std::endl;
                while (!(*thread_args[i].start_flags)[group_num]) {
                    // Wait until the start flag is set
                    // std::cout<<"what is up"<<std::endl;
                }
                while(!(*thread_args[i].end_flags)[group_num]){
                    // Perform the locking operations
                    thread_args[i].lock->lock(thread_args[i].thread_id);
                    thread_args[i].stats.num_iterations++;
                    (*counter)++;
                    // Critical section code goes here
                    thread_args[i].lock->unlock(thread_args[i].thread_id);

                }
                // std::cout<<"id "<<thread_args[i].thread_id<< " has endedwhat is up"<<std::endl;
                thread_args[i].stats.num_iterations--;
        });
    }

    schedule_flags(start_flags, end_flags, run_time, num_groups);

    // Wait for all threads to finish
    for (auto& thread : threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    *counter -= num_threads;

    int expectedIterations = 0;
    for (int i =0; i<num_threads; i++){
        expectedIterations+=thread_args[i].stats.num_iterations;
    }

    if (*counter != expectedIterations) {
        // The mutex did not work.
        fprintf(stderr, "Mutex %s failed; *counter != num_threads * num_iterations (%d!=%d)\n", lock->name().c_str(), *counter, expectedIterations);
        return 1;
    }

    // Cleanup resources
    lock->destroy(); // Cleanup the lock resources
    free((void *)counter);

    // Destroy the lock 
    delete lock; // Assuming lock was dynamically allocated

    // Output benchmark results

    for (auto& args : thread_args) {
        report_thread_stats(&args.stats, csv); // Report latency for each thread
    }

    // record_rusage(); // Record resource usage
    // report_latency(&args); // Report latency if needed
}

void schedule_flags(std::shared_ptr<std::atomic<bool>*> start_flags, std::shared_ptr<std::atomic<bool>*> end_flags, std::chrono::nanoseconds run_time, int num_groups){
    for (int i=0; i<num_groups; i++){

        std::atomic_thread_fence(std::memory_order_seq_cst);
        // std::cout<<"whats up "<<std::endl;
        (*start_flags)[i]=true;
        std::this_thread::sleep_for(run_time);
        (*end_flags)[i]=true;

        std::atomic_thread_fence(std::memory_order_seq_cst);
        std::cout<<"group "<<i<<" has ended"<<std::endl;
    }
    
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <mutex_name> <num_threads> <run_time_per_group> <num_groups> <flags>]\n", argv[0]);
        return 1;
    }

    // First, take in command line arguments
    char *mutex_name = nullptr;
    int num_threads = -1;
    bool csv = false;
    bool thread_level = false;
    int run_time =-1;
    int num_groups =-1;

    for (int i = 1; i < argc; i++) 
    {
        // First, check if the argument is a flag, which can be placed anywhere.
        if (strcmp(argv[i], "--csv") == 0 || strcmp(argv[i], "-c") == 0) {
            csv = true;
        } else if (strcmp(argv[i], "--thread-level") == 0 || strcmp(argv[i], "-t") == 0) {
            thread_level = true;
        } else if (mutex_name == nullptr) {
            mutex_name = argv[i];
        } else if (num_threads == -1) {
            num_threads = atoi(argv[i]);
        } else if (run_time == -1){
            run_time = atoi(argv[i]);
        } else if (num_groups==-1){
            num_groups = atoi(argv[i]);
        } else {
            fprintf(stderr, "Unrecognized command line argument: %s\n", argv[i]);
            return 1;
        }
    }

    if (num_threads%num_groups!=0){
        fprintf(stderr, "Number of threads must be evenly divisible by number of groups");
        return 1;
    }

    // Create a lock instance (using Pthread lock as an example)
    // This section is going to be annoying to change every time we add a new mutex.
    SoftwareMutex* lock;
    if (strcmp(mutex_name, "pthread") == 0) {
        lock = new Pthread();
    } else if (strcmp(mutex_name, "cpp_std") == 0) {
        lock = new CPPMutex();
    } else if (strcmp(mutex_name, "boost") == 0) {
        lock = new BoostMutex();
    } else if (strcmp(mutex_name, "dijkstra") == 0) {
        lock = new DijkstraMutex();
    } else if (strcmp(mutex_name, "spin") == 0) {
        lock = new SpinLock();
    } else if (strcmp(mutex_name, "nsync") == 0){
        lock = new NSync();
    } else if (strcmp(mutex_name, "exp_spin") == 0){
        lock = new ExponentialSpinLock();
    } else if (strcmp(mutex_name, "bakery") == 0){
        lock = new BakeryMutex();
    } else {
        fprintf(stderr, "Unrecognized mutex name: %s"
                "\nValid names are 'pthread', 'cpp_std', 'boost', 'dijkstra',"
                "'spin', 'nsync', 'exp_spin', and 'bakery'\n", mutex_name);
        return 1;
    }    
    
    // Run the max contention benchmark
    return grouped_contention_bench(num_threads, std::chrono::nanoseconds(run_time), num_groups, csv, lock);
}