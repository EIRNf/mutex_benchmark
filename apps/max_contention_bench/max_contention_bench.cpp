#include <time.h>

#include "max_contention_bench.hpp"
#include "bench_utils.hpp"

#include "lock.hpp"
#include "pthread_lock.cpp"
#include "cpp_std_mutex.cpp"
#include "boost_lock.cpp"
#include "dijkstra_lock.cpp"
#include "dijkstra_nonatomic_lock.cpp"
#include "spin_lock.cpp"
#include "nsync_lock.cpp"
#include "exp_spin_lock.cpp"
#include "bakery_mutex.cpp"
#include "mcs_lock.cpp"
#include "mcs_volatile_lock.cpp"
#include "mcs_malloc_lock.cpp"
#include "knuth_lock.cpp"
#include "peterson_lock.cpp"


int max_contention_bench(int num_threads, std::chrono::seconds run_time, bool csv, bool thread_level, bool no_output, int max_noncritical_delay_ns, SoftwareMutex* lock) {

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
    std::shared_ptr<std::atomic<bool>> end_flag = std::make_shared<std::atomic<bool>>(false);
    volatile int *counter = (volatile int*)malloc(sizeof(int));
    // Create an array of thread arguments
    std::vector<per_thread_args> thread_args(num_threads);
    for (int i = 0; i < num_threads; ++i) {
        thread_args[i].thread_id = i;
        thread_args[i].stats.run_time = run_time;
        thread_args[i].lock = lock; // Pass the lock to each thread
    }

    // Create an array of threads
    std::vector<std::thread> threads(num_threads);
    for (int i = 0; i < num_threads; ++i) {
        thread_args[i].start_flag = start_flag; // Share the start flag with each thread
        thread_args[i].end_flag = end_flag; // Share the start flag with each thread
        // I put the if statement here rather than inside the thread code so that fewer conditionals have to be checked
        // in the runtime-importants section
        if (thread_level) {
            // Measure how long each thread takes to finish
            threads[i] = std::thread([&thread_args, i, counter]() {

                // Record the thread ID
                thread_args[i].stats.thread_id = thread_args[i].thread_id;
            
                // Each thread will run this function
                while (!*thread_args[i].start_flag) {
                    // Wait until the start flag is set
                }

                // Perform the locking operations
                while (!*thread_args[i].end_flag) {
                    thread_args[i].lock->lock(thread_args[i].thread_id);
                    thread_args[i].stats.num_iterations++;
                    (*counter)++; // Critical section
                    thread_args[i].lock->unlock(thread_args[i].thread_id);
                }
            });
        } else {
            // Measure individual lock operations
            // May be affected by how long the clock takes to read
            threads[i] = std::thread([&thread_args, i, counter, max_noncritical_delay_ns]() {

                // Record the thread ID
                thread_args[i].stats.thread_id = thread_args[i].thread_id;
                init_lock_timer(&thread_args[i].stats);
            
                // Each thread will run this function
                while (!*thread_args[i].start_flag) {
                    // Wait until the start flag is set
                }

                struct timespec delay = { 0, 0 };
                struct timespec _remaining;

                // Perform the locking operations
                while (!*thread_args[i].end_flag) {
                    start_lock_timer(&thread_args[i].stats);
                    thread_args[i].lock->lock(thread_args[i].thread_id);
                    (*counter)++; // Critical section
                    thread_args[i].lock->unlock(thread_args[i].thread_id);
                    end_lock_timer(&thread_args[i].stats);
                    
                    // Noncritical section
                    delay.tv_nsec = rand() % max_noncritical_delay_ns;
                    nanosleep(&delay, &_remaining);
                    thread_args[i].stats.num_iterations++;
                }
            });
        }
    }

    // Start the threads
    *start_flag = true; // Set the start flag to signal the threads to begin
    std::this_thread::sleep_for(run_time);
    *end_flag = true;

    // Wait for all threads to finish
    for (auto& thread : threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    int expected_iterations = 0;
    for (int i =0; i<num_threads; i++){
        expected_iterations+=thread_args[i].stats.num_iterations;
    }

    if (*counter != expected_iterations) {
        // The mutex did not work.
        fprintf(stderr, "Mutex %s failed; *counter != num_threads * num_iterations (%d!=%d)\n", lock->name().c_str(), *counter, expected_iterations);
        return 1;
    }

    // Cleanup resources
    lock->destroy(); // Cleanup the lock resources
    free((void *)counter);

    // Destroy the lock 
    delete lock; // Assuming lock was dynamically allocated

    // Output benchmark results

    if (!no_output) {
        for (auto& args : thread_args) {
            report_thread_latency(&args.stats, csv, thread_level);
            if (!thread_level) {
                destroy_lock_timer(&args.stats);
            }
        }
    }

    return 0;
    // record_rusage(); // Record resource usage
    // report_latency(&args); // Report latency if needed
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <mutex_name> <num_threads> <num_iterations> [<flags>]\n", argv[0]);
        return 1;
    }

    // First, take in command line arguments
    char *mutex_name = nullptr;
    int num_threads = -1;
    int run_time = -1;
    int max_noncritical_delay_ns = -1;
    bool csv = false;
    bool thread_level = false;
    bool no_output = false;

    for (int i = 1; i < argc; i++) 
    {
        // First, check if the argument is a flag, which can be placed anywhere.
        if (strcmp(argv[i], "--csv") == 0 || strcmp(argv[i], "-c") == 0) {
            csv = true;
        } else if (strcmp(argv[i], "--thread-level") == 0 || strcmp(argv[i], "-t") == 0) {
            thread_level = true;
        } else if (strcmp(argv[i], "--no-output") == 0 || strcmp(argv[i], "-n") == 0) {
            no_output = true;
        } else if (mutex_name == nullptr) {
            mutex_name = argv[i];
        } else if (num_threads == -1) {
            num_threads = atoi(argv[i]);
        } else if (run_time == -1) {
            run_time = atoi(argv[i]);
        } else if (run_time == -1) {
            run_time = atoi(argv[i]);
        } else if (max_noncritical_delay_ns == -1) {
            max_noncritical_delay_ns = atoi(argv[i]);
        } else {
            fprintf(stderr, "Unrecognized command line argument: %s\n", argv[i]);
            return 1;
        }
    }

    // Default arguments
    if (max_noncritical_delay_ns <= 0) {
        max_noncritical_delay_ns = 1;
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
    } else if (strcmp(mutex_name, "dijkstra_nonatomic") == 0){
        lock = new DijkstraNonatomicMutex();
    } else if (strcmp(mutex_name, "mcs") == 0){
        lock = new MCSMutex();
    } else if (strcmp(mutex_name, "mcs_volatile") == 0){
        lock = new MCSVolatileMutex();
    } else if (strcmp(mutex_name, "mcs_malloc") == 0){
        lock = new MCSMallocMutex();
    } else if (strcmp(mutex_name, "knuth") == 0){
        lock = new KnuthMutex();
    } else if (strcmp(mutex_name, "peterson") == 0){
        lock = new PetersonMutex();
    } else {
        fprintf(stderr, "Unrecognized mutex name: %s"
                "\nValid names are 'pthread', 'cpp_std', 'boost', 'dijkstra',"
                "'spin', 'nsync', 'exp_spin', 'bakery', 'dijkstra_nonatomic', 'mcs', 'knuth', and 'peterson'\n", mutex_name);
        return 1;
    }    
    
    // Run the max contention benchmark
    max_contention_bench(num_threads, std::chrono::seconds(run_time), csv, thread_level, no_output, max_noncritical_delay_ns, lock);

    return 0;
}