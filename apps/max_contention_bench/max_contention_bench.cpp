#include <time.h>

#include "max_contention_bench.hpp"
#include "bench_utils.hpp"

#include "lock.hpp"
#include "pthread_lock.cpp"
#include "cpp_std_mutex.cpp"
#include "boost_lock.cpp"
#include "dijkstra_lock.cpp"
#include "dijkstra_nonatomic_lock.cpp"
#include "spin_lock.hpp"
#include "nsync_lock.cpp"
#include "exp_spin_lock.cpp"
#include "bakery_mutex.cpp"
#include "bakery_nonatomic_mutex.cpp"
#include "lamport_lock.cpp"
#include "mcs_lock.cpp"
#include "mcs_volatile_lock.cpp"
#include "mcs_malloc_lock.cpp"
#include "knuth_lock.cpp"
#include "peterson_lock.cpp"
#include "boulangerie.cpp"
#include "ticket_lock.cpp"
#include "threadlocal_ticket_lock.cpp"
#include "ring_ticket_lock.cpp"
#include "null_mutex.cpp"
#include "halfnode_lock.cpp"
#include "hopscotch_lock.cpp"
#include "clh_lock.cpp"
#include "linear_cas_elevator.cpp"
#include "tree_cas_elevator.cpp"
#include "linear_bl_elevator.cpp"
#include "tree_bl_elevator.cpp"
#include "burns_lamport_lock.hpp"
#include "futex_mutex.cpp"
#include "elevator_mutex.hpp"

int max_contention_bench(
    int num_threads, 
    double run_time, 
    bool csv, 
    bool thread_level, 
    bool no_output, 
    int max_critical_delay_iterations, 
    int max_noncritical_delay_iterations, 
    SoftwareMutex* lock
) {
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
            threads[i] = std::thread([&thread_args, i, counter, max_critical_delay_iterations, max_noncritical_delay_iterations]() {

                // Record the thread ID
                thread_args[i].stats.thread_id = thread_args[i].thread_id;
                init_lock_timer(&thread_args[i].stats);
            
                // Each thread will run this function
                while (!*thread_args[i].start_flag) {
                    // Wait until the start flag is set
                }

                // struct timespec delay_critical = { 0, 0 };
                // struct timespec delay_noncritical = { 0, 0 };
                // struct timespec _remaining;

                // Perform the locking operations
                while (!*thread_args[i].end_flag) {
                    // Lock
                    start_lock_timer(&thread_args[i].stats);
                    thread_args[i].lock->lock(thread_args[i].thread_id);

                    // Critical section
                    (*counter)++; // Nonatomic work
                    busy_sleep(rand() % max_critical_delay_iterations);
                    // if (max_critical_delay_iterations != 1) { // Delay
                    //     delay_critical.tv_nsec = rand() % max_critical_delay_iterations;
                    //     nanosleep(&delay_critical, &_remaining);
                    // }

                    // Unlock
                    thread_args[i].lock->unlock(thread_args[i].thread_id);
                    end_lock_timer(&thread_args[i].stats);
                    
                    // Noncritical section
                    // if (max_noncritical_delay_iterations != 1) {
                    //     delay_noncritical.tv_nsec = rand() % max_noncritical_delay_iterations;
                    //     nanosleep(&delay_noncritical, &_remaining);
                    // }
                    busy_sleep(rand() % max_noncritical_delay_iterations);
                    thread_args[i].stats.num_iterations++;
                }
            });
        }
    }

    // Start the threads
    *start_flag = true; // Set the start flag to signal the threads to begin
    std::this_thread::sleep_for(std::chrono::duration<double>(run_time));
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

    if (*counter != expected_iterations && lock->name() != "null") {
        // The mutex did not work (null mutex is for testing benchmarking and should always fail.)
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
    double run_time = -1;
    int max_critical_delay_iterations = -1;
    int max_noncritical_delay_iterations = -1;
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
        } else if (run_time < 0) {
            run_time = atof(argv[i]);
        } else if (max_critical_delay_iterations == -1) {
            max_critical_delay_iterations = atoi(argv[i]);
        } else if (max_noncritical_delay_iterations == -1) {
            max_noncritical_delay_iterations = atoi(argv[i]);
        } else {
            fprintf(stderr, "Unrecognized command line argument: %s\n", argv[i]);
            return 1;
        }
    }

    // Default arguments
    assert(max_critical_delay_iterations < 100000000);
    assert(max_noncritical_delay_iterations < 100000000);
    if (max_critical_delay_iterations <= 0) {
        max_critical_delay_iterations = 1;
    }
    if (max_noncritical_delay_iterations <= 0) {
        max_noncritical_delay_iterations = 1;
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
    } else if (strcmp(mutex_name, "bakery_nonatomic") == 0){
        lock = new BakeryNonAtomicMutex();
    } else if (strcmp(mutex_name, "lamport") ==0){
        lock = new LamportLock();
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
    } else if (strcmp(mutex_name, "ticket") == 0){
        lock = new TicketMutex();
    } else if (strcmp(mutex_name, "threadlocal_ticket") == 0){
        lock = new ThreadlocalTicketMutex();
    } else if (strcmp(mutex_name, "ring_ticket") == 0){
        lock = new RingTicketMutex();
    } else if (strcmp(mutex_name, "null") == 0){
        lock = new NullMutex();
    } else if (strcmp(mutex_name, "halfnode") == 0){
        lock = new HalfnodeMutex();
    } else if (strcmp(mutex_name, "hopscotch") == 0){
        lock = new HopscotchMutex();
    } else if (strcmp(mutex_name, "clh") == 0){
        lock = new CLHMutex();
    } else if (strcmp(mutex_name, "boulangerie") == 0) {
        lock = new Boulangerie();
    } else if (strcmp(mutex_name, "linear_cas_elevator") == 0) {
        lock = new LinearCASElevatorMutex();
    } else if (strcmp(mutex_name, "tree_cas_elevator") == 0) {
        lock = new TreeCASElevatorMutex();
    } else if (strcmp(mutex_name, "linear_bl_elevator") == 0) {
        lock = new LinearBLElevatorMutex();
    } else if (strcmp(mutex_name, "tree_bl_elevator") == 0) {
        lock = new TreeBLElevatorMutex();
    } else if (strcmp(mutex_name, "burns_lamport") == 0) {
        lock = new BurnsLamportMutex();
    } else if (strcmp(mutex_name, "futex") == 0) {
        lock = new FutexLock();
    } else if (strcmp(mutex_name, "elevator") == 0) {
        lock = new ElevatorMutex();
    } else {
        fprintf(stderr, "Unrecognized mutex name: %s"
                "\nValid names include 'pthread', 'cpp_std', 'boost', 'dijkstra',"
                " 'spin', 'nsync', 'exp_spin', 'bakery', 'dijkstra_nonatomic', 'mcs',"
                " 'knuth', 'peterson', 'ticket', 'threadlocal_ticket', 'ring_ticket',"
                " 'halfnode', 'hopscotch', 'clh', 'linear_cas_elevator', 'tree_cas_elevator',"
                " 'linear_bl_elevator', 'tree_bl_elevator', 'burns_lamport', 'futex'\n", mutex_name);
        return 1;
    }    

    // Run the max contention benchmark
    max_contention_bench(num_threads, run_time, csv, thread_level, no_output, max_critical_delay_iterations, max_noncritical_delay_iterations, lock);

    return 0;
}