# scripts/runner.py

from .constants import Constants
from .logger    import logger
import subprocess


# TODO: save data files instead of deleting them every time (in their own folder)

def get_data_file_name(mutex_name, i, threads, rusage=False):
    if not(rusage):
        return f"{Constants.data_folder}/{mutex_name}-{threads}-{i}.csv"
    else:
        return f"{Constants.data_folder}/{mutex_name}-{threads}-{i}-rusage.csv"

def get_command(mutex_name, threads, *, csv=True, thread_level=False, rusage=False):
    if Constants.bench=="max":
        cmd = [
            Constants.executable,
            mutex_name,
            str(threads),
            str(Constants.bench_n_seconds),
            str(Constants.noncritical_delay)
        ]
    else:
        cmd = [Constants.executable,
            mutex_name,
            str(threads),
            str(Constants.bench_n_seconds)]
    if Constants.bench == "grouped":
        cmd.append(str(Constants.groups))
    if Constants.bench=='lru':
        cmd.append(str(Constants.keys))
    if csv:
        cmd.append("--csv")
    if thread_level:

        cmd.append("--thread-level")
    if rusage:
        cmd.append("--rusage")

    if Constants.low_contention:
        cmd.append("--low-contention")
        if Constants.stagger_ms and Constants.stagger_ms > 0:
            cmd += ["--stagger-ms", str(Constants.stagger_ms)]

    logger.debug(f"Bench command: {cmd}")
    print(cmd)
    return cmd


def run_experiment_multi_threaded():
    for mutex_name in Constants.mutex_names:
        procs = []
        for i in range(Constants.n_program_iterations):
            fname = get_data_file_name(mutex_name, i, Constants.bench_n_threads)
            subprocess.run(["rm", "-f", fname])
            p = subprocess.Popen(
                get_command(
                    mutex_name,
                    Constants.bench_n_threads,
                    csv=True,
                    thread_level=True
                ),
                stdout=subprocess.PIPE
            )
            procs.append((fname, p))

        for fname, p in procs:
            p.wait()
            output = p.stdout.read()
            with open(fname, "wb") as f:
                f.write(output)

def run_experiment_single_threaded():
    for i in range(Constants.n_program_iterations):
        for mutex_name in Constants.mutex_names:
            logger.info(f"Running {mutex_name=} | iteration {i}")
            fname = get_data_file_name(mutex_name, i, Constants.bench_n_threads)
            subprocess.run(["rm", "-f", fname])
            proc = subprocess.run(
                get_command(
                    mutex_name,
                    Constants.bench_n_threads,
                    csv=True,
                    thread_level=Constants.thread_level
                ),
                stdout=subprocess.PIPE
            )
            with open(fname, "wb") as f:
                f.write(proc.stdout)

def run_experiment_lock_level_single_threaded():
    for i in range(Constants.n_program_iterations):
        for mutex_name in Constants.mutex_names:
            logger.info(f"Running lock-level {mutex_name=} | iteration {i}")
            fname = get_data_file_name(mutex_name, i, Constants.bench_n_threads)
            subprocess.run(["rm", "-f", fname])
            proc = subprocess.run(
                get_command(
                    mutex_name,
                    Constants.bench_n_threads,
                    csv=True,
                    thread_level=False
                ),
                stdout=subprocess.PIPE
            )
            with open(fname, "wb") as f:
                f.write(proc.stdout)


def run_experiment_iter_v_threads_single_threaded(rusageGet=False):
    for threads in range(
        Constants.threads_start,
        Constants.threads_end,
        Constants.threads_step
    ):
        for i in range(Constants.n_program_iterations):
            for mutex_name in Constants.mutex_names:
                logger.info(
                    f"Running {mutex_name=} | threads={threads} | iteration {i}"
                )
                fname = get_data_file_name(mutex_name, i, threads, rusage=rusageGet)
                subprocess.run(["rm", "-f", fname])
                proc = subprocess.run(
                    get_command(mutex_name, threads, 
                                          csv=True, thread_level=True, 
                                          rusage=rusageGet),
                    stdout=subprocess.PIPE
                )
                with open(fname, "wb") as f:
                    f.write(proc.stdout)

def run_grouped_experiment_iter_v_threads_single_threaded(rusageGet=False):
    for threads in range(Constants.threads_start, Constants.threads_end, Constants.threads_step):
        for i in range(Constants.n_program_iterations):
            for mutex_name in Constants.mutex_names:
                logger.info(f"{mutex_name=} | {threads=} | {i=}")
                data_file_name = get_data_file_name(mutex_name, i, threads, rusage=rusageGet)
                subprocess.run(["rm", "-f", data_file_name])
                
                command = get_command(mutex_name, threads, csv=True, thread_level=True, rusage=rusageGet)
                print(command)
                thread=subprocess.run(command, stdout=subprocess.PIPE)
                csv_data = thread.stdout
                with open(data_file_name, "wb") as data_file:
                    data_file.write(csv_data)

