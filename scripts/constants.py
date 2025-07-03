import logging

class Constants:
    class Defaults:
        MUTEX_NAMES = ["dijkstra", "bakery", "spin", "exp_spin", "nsync", "boost", "cpp_std", "pthread", "mcs", "knuth", "peterson"]
        EXECUTABLE_NAME = "max_contention_bench"
        BENCH_N_THREADS = "10"
        BENCH_N_SECONDS = "1"
        N_PROGRAM_ITERATIONS = 10
        DATA_FOLDER = "./data/generated"
        LOGS_FOLDER = "./data/logs"
        EXECUTABLE = f"./build/apps/{EXECUTABLE_NAME}/{EXECUTABLE_NAME}"
        MULTITHREADED = False
        THREAD_LEVEL = True
        SCATTER = False
        LOG = logging.INFO
        SKIP = 1
        MAX_N_POINTS = 1000
    
