#include "max_contention_bench.hpp"
#include "bench_utils.hpp"

#include "lock.hpp"
#include "pthread_lock.cpp"
#include "cpp_std_mutex.cpp"
#include "boost_lock.cpp"
#include "dijkstra_lock.cpp"
#include "spin_lock.cpp"
#include "nsync_lock.cpp"


int max_contention_bench(int num_threads, int num_iterations, bool csv, SoftwareMutex* lock) {

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
    std::shared_ptr<std::atomic<bool>> start_flag = std::make_shared<std::atomic<bool>>(false);
    volatile int *counter = (volatile int*)malloc(sizeof(int));

    // Create an array of thread arguments
    std::vector<per_thread_args> thread_args(num_threads);
    for (int i = 0; i < num_threads; ++i) {
        thread_args[i].thread_id = i;
        thread_args[i].stats.num_iterations = num_iterations;
        thread_args[i].lock = lock; // Pass the lock to each thread
    }

    // Create an array of threads
    std::vector<std::thread> threads(num_threads);
    for (int i = 0; i < num_threads; ++i) {
        thread_args[i].start_flag = start_flag; // Share the start flag with each thread
        threads[i] = std::thread([&thread_args, i, counter]() {

            // Record the thread ID
            thread_args[i].stats.thread_id = thread_args[i].thread_id;
        
            // Each thread will run this function
            while (!*thread_args[i].start_flag) {
                // Wait until the start flag is set
            }

            // Start the timer for this thread
            start_timer(&thread_args[i].stats);

            // Perform the locking operations
            for (int j = 0; j < thread_args[i].stats.num_iterations; ++j) {
                thread_args[i].lock->lock(thread_args[i].thread_id);
                (*counter)++;
                // Critical section code goes here
                thread_args[i].lock->unlock(thread_args[i].thread_id);
            }

            // End the timer for this thread
            end_timer(&thread_args[i].stats);
        });
    }

    // Start the threads
    *start_flag = true; // Set the start flag to signal the threads to begin

    // Wait for all threads to finish
    for (auto& thread : threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    if (*counter != num_threads * num_iterations) {
        // The mutex did not work.
        fprintf(stderr, "Mutex %s failed; *counter != num_threads * num_iterations (%d!=%d)\n", lock->name().c_str(), *counter, num_threads * num_iterations);
        return 1;
    }

    // Cleanup resources
    lock->destroy(); // Cleanup the lock resources
    free((void *)counter);

    // Destroy the lock 
    delete lock; // Assuming lock was dynamically allocated

    // Output benchmark results

    for (auto& args : thread_args) {
        report_thread_latency(&args.stats, csv); // Report latency for each thread
    }

    // record_rusage(); // Record resource usage
    // report_latency(&args); // Report latency if needed
    return 0;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <mutex_name> <num_threads> <num_iterations> [<flags>]\n", argv[0]);
        return 1;
    }

    // First, take in command line arguments
    char *mutex_name = nullptr;
    int num_threads = -1;
    int num_iterations = -1;
    bool csv = false;

    for (int i = 1; i < argc; i++) 
    {
        // First, check if the argument is a flag, which can be placed anywhere.
        if (strcmp(argv[i], "--csv") == 0 || strcmp(argv[i], "-c") == 0) {
            csv = true;
        } else if (mutex_name == nullptr) {
            mutex_name = argv[i];
        } else if (num_threads == -1) {
            num_threads = atoi(argv[i]);
        } else if (num_iterations == -1) {
            num_iterations = atoi(argv[i]);
        } else {
            fprintf(stderr, "Unrecognized command line argument: %s\n", argv[i]);
            return 1;
        }
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
    } else {
        fprintf(stderr, "Unrecognized mutex name: %s\nValid names are 'pthread', 'cpp_std', and 'boost'\n", mutex_name);
        return 1;
    }    
    
    // Run the max contention benchmark
    return max_contention_bench(num_threads, num_iterations, csv, lock);
}