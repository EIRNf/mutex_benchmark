#include "grouped_contention_bench.hpp"
#include "bench_utils.hpp"

#include "lock.hpp"

#include "cpp_std_mutex.cpp"
#include "boost_lock.cpp"
#include "dijkstra_lock.cpp"
#include "dijkstra_nonatomic_lock.cpp"
#include "dijkstra_nonatomic_sleeper_lock.cpp"
#include "spin_lock.cpp"
#include "exp_spin_lock.cpp"
#include "nsync_lock.cpp"
#include "bakery_mutex.cpp"
#include "bakery_nonatomic_mutex.cpp"
#include "lamport_lock.cpp"
#include "mcs_lock.cpp"
#include "mcs_volatile_lock.cpp"
#include "mcs_malloc_lock.cpp"
#include "knuth_lock.cpp"
#include "peterson_lock.cpp"
#include "boulangerie.cpp"
#include "wait_spin_lock.cpp"
#include "lamport_sleeper_lock.cpp"
#include "system_lock.cpp"
#include "mcs_sleeper_lock.cpp"
#include "knuth_sleeper_lock.cpp"
#include "yang_lock.cpp"
#include "yang_sleeper_lock.cpp"
#include "szymanski.cpp"


int grouped_contention_bench(int num_threads, std::chrono::nanoseconds run_time, int num_groups, bool csv, bool rusage, SoftwareMutex* lock) {


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
        threads[i] = std::thread([&, i]() {

                // Record the thread ID
                thread_args[i].stats.thread_id = thread_args[i].thread_id;
            
                // Each thread will run this function

                int group_num = num_groups*(((double)thread_args[i].thread_id)/((double)num_threads));
                while (!(*thread_args[i].start_flags)[group_num]) {
                    // Wait until the start flag is set
                }
                while(!(*thread_args[i].end_flags)[group_num]){
                    // Perform the locking operations
                    thread_args[i].lock->lock(thread_args[i].thread_id);
                    thread_args[i].stats.num_iterations++;
                    lock->criticalSection(i);
                    thread_args[i].lock->unlock(thread_args[i].thread_id);

                }
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


    if (rusage){
        record_rusage(csv);
    }

    // Cleanup resources
    lock->destroy(); // Cleanup the lock resources

    // Destroy the lock 
    delete lock; // Assuming lock was dynamically allocated

    // Output benchmark results

    if (!rusage){
        for (auto& args : thread_args) {
            report_thread_latency(&args.stats, csv, true); // Report latency for each thread
        }
    }

    // record_rusage(); // Record resource usage
    // report_latency(&args); // Report latency if needed
}

void schedule_flags(std::shared_ptr<std::atomic<bool>*> start_flags, std::shared_ptr<std::atomic<bool>*> end_flags, std::chrono::nanoseconds run_time, int num_groups){
    for (int i=0; i<num_groups; i++){

        (*start_flags)[i]=true;
        std::this_thread::sleep_for(run_time);
        (*end_flags)[i]=true;

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
    bool rusage = false;

    for (int i = 1; i < argc; i++) 
    {
        // First, check if the argument is a flag, which can be placed anywhere.
        if (strcmp(argv[i], "--csv") == 0 || strcmp(argv[i], "-c") == 0) {
            csv = true;
        } else if (strcmp(argv[i], "--thread-level") == 0 || strcmp(argv[i], "-t") == 0) {
            thread_level = true;
        } else if (strcmp(argv[i], "--rusage") == 0) {
            rusage=true;
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
    SoftwareMutex* lock = nullptr;
    if      (strcmp(mutex_name, "system") == 0)                     lock = new System();
    else if (strcmp(mutex_name, "cpp_std") == 0)                    lock = new CPPMutex();
    else if (strcmp(mutex_name, "boost") == 0)                      lock = new BoostMutex();
    else if (strcmp(mutex_name, "dijkstra") == 0)                   lock = new DijkstraMutex();
    else if (strcmp(mutex_name, "dijkstra_nonatomic") == 0)         lock = new DijkstraNonatomicMutex();
    else if (strcmp(mutex_name, "dijkstra_nonatomic_sleeper") == 0) lock = new DijkstraNonatomicSleeperMutex();
    else if (strcmp(mutex_name, "spin") == 0)                       lock = new SpinLock();
    else if (strcmp(mutex_name, "exp_spin") == 0)                   lock = new ExponentialSpinLock();
    else if (strcmp(mutex_name, "wait_spin") == 0)                  lock = new ExponentialSpinLock();
    else if (strcmp(mutex_name, "nsync") == 0)                      lock = new NSync();
    else if (strcmp(mutex_name, "bakery") == 0)                     lock = new BakeryMutex();
    else if (strcmp(mutex_name, "bakery_nonatomic") == 0)           lock = new BakeryNonAtomicMutex();
    else if (strcmp(mutex_name, "lamport") == 0)                    lock = new LamportLock();
    else if (strcmp(mutex_name, "lamport_sleeper") == 0)            lock = new LamportSleeperLock();
    else if (strcmp(mutex_name, "mcs") == 0)                        lock = new MCSMutex();
    else if (strcmp(mutex_name, "mcs_volatile") == 0)               lock = new MCSVolatileMutex();
    else if (strcmp(mutex_name, "mcs_malloc") == 0)                 lock = new MCSMallocMutex();
    else if (strcmp(mutex_name, "mcs_sleeper") == 0)                lock = new MCSSleeperMutex();
    else if (strcmp(mutex_name, "knuth") == 0)                      lock = new KnuthMutex();
    else if (strcmp(mutex_name, "knuth_sleeper") == 0)              lock = new KnuthSleeperMutex(); //does not work as of 7/14/25
    else if (strcmp(mutex_name, "peterson") == 0)                   lock = new PetersonMutex();
    else if (strcmp(mutex_name, "boulangerie") == 0)                lock = new Boulangerie();
    else if (strcmp(mutex_name, "szymanski") == 0)                  lock = new SzymanskiLock();
    else if (strcmp(mutex_name, "yang") == 0)                       lock = new YangMutex();
    else if (strcmp(mutex_name, "yang_sleeper") == 0)               lock = new YangSleeperMutex();
    else {
        fprintf(stderr,
            "Unrecognized mutex '%s'\n", mutex_name
        );
        return 1;
    }
    
    // Run the max contention benchmark
    return grouped_contention_bench(num_threads, std::chrono::nanoseconds(run_time), num_groups, csv, rusage, lock);
}