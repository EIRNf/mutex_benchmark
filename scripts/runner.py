# scripts/runner.py

from .constants import Constants
from .logger    import logger
import subprocess
import os
import re

def get_data_file_name(mutex_name, i, **iter_kwargs):
    """Return the CSV path for one benchmark point.

    iter_kwargs should contain only the swept variable (e.g. threads=4) —
    not the rusage flag; that is encoded in the capture segment instead.
    """
    capture = getattr(Constants, "capture", "latency")
    alloc_tag = ""
    if Constants.hardware_cxl:
        alloc_tag = "-cxl-hw"
    elif Constants.software_cxl:
        alloc_tag = "-cxl-sw"

    name_root = (
        f"{Constants.data_folder}/"
        f"{mutex_name}-{Constants.bench}-{capture}{alloc_tag}-rep{i}"
    )
    for key, value in iter_kwargs.items():
        name_root += f"-{key}={value}"
    return name_root + ".csv"

def get_command(mutex_name, *, threads=None, csv=True, thread_level=False, critical_delay=None, noncritical_delay=None, rusage=False):
    if threads is None:
        threads = Constants.bench_n_threads
    if critical_delay is None:
        critical_delay = Constants.critical_delay
    if noncritical_delay is None:
        noncritical_delay = Constants.noncritical_delay

    if Constants.bench.startswith("tigon"):
        run_sec = max(1, int(Constants.bench_n_seconds))
        # Use with_lock.sh to inject the mutex via LD_PRELOAD
        cmd = [
            "./scripts/with_lock.sh",
            "-n", str(max(128, threads * 4)),
            mutex_name,
            Constants.executable,
            "--logtostderr=1",
            "--id=0",
            "--servers=127.0.0.1:1234",
            f"--threads={threads}",
            f"--partition_num={threads}",
            f"--time_to_run={run_sec}",
            "--time_to_warmup=1",
            "--protocol=TwoPL",
        ]
        if "tpcc" in Constants.bench:
            cmd.extend([
                "--query=mixed",
                "--neworder_dist=10",
                "--payment_dist=15",
            ])
        else:
            cmd.extend([
                "--query=rmw",
                "--read_write_ratio=80",
                "--keys=200000",
            ])
        logger.debug(f"Bench command: {cmd}")
        return cmd

    cmd = [
        Constants.executable, 
        mutex_name, 
        str(threads), 
        str(Constants.bench_n_seconds), 
    ]
    if Constants.software_cxl:
        cmd.insert(0, "sudo")
    if Constants.groups:
        cmd.append(str(Constants.groups))

    # Only max_contention_bench simulates critical/noncritical-section delay;
    # min_/grouped_contention_bench don't recognize these flags and will
    # error out if passed.
    if Constants.bench == 'max':
        if critical_delay != -1:
            cmd += ["--critical-delay", str(critical_delay)]
        if noncritical_delay != -1:
            cmd += ["--noncritical-delay", str(noncritical_delay)]

    if csv:
        cmd.append("--csv")
    if thread_level:
        cmd.append("--thread-level")

    # min_contention_bench doesn't support --rusage.
    if rusage and Constants.bench in ('max', 'grouped'):
        cmd.append("--rusage")

    # grouped_contention_bench doesn't support staggered/low-contention startup.
    if Constants.low_contention and Constants.bench in ('max', 'min'):
        cmd.append("--low-contention")
        if Constants.stagger_ms and Constants.stagger_ms > 0:
            cmd += ["--stagger-ms", str(Constants.stagger_ms)]

    logger.debug(f"Bench command: {cmd}")
    return cmd


def _parse_tigon_output_to_csv(stdout_bytes, stderr_bytes, threads: int, seconds: float) -> bytes:
    """Parse Tigon's commit counts and format as CSV rows matching other benchmarks."""
    out = stdout_bytes.decode("utf-8", errors="replace")
    err = stderr_bytes.decode("utf-8", errors="replace")
    combined = out + "\n" + err

    total_commit = 0
    match = re.search(r"total_commit:\s*(\d+)", combined)
    if match:
        total_commit = int(match.group(1))
    else:
        avg_match = re.search(r"average commit:\s*([\d.]+)", combined)
        if avg_match:
            total_commit = int(float(avg_match.group(1)) * seconds)
        else:
            logger.warning("Could not parse total_commit or average commit from Tigon output")

    lines = []
    per_thread = total_commit // threads if threads > 0 else 0
    rem = total_commit % threads if threads > 0 else 0
    for t in range(threads):
        cnt = per_thread + (1 if t < rem else 0)
        lines.append(f"{t},{seconds:.6f},{cnt}\n")
    return "".join(lines).encode("utf-8")


def _run_command_to_csv(command, timeout_s: float, threads: int = 1, seconds: float = 1.0):
    """Run *command* and return its stdout bytes.

    Raises RuntimeError on non-zero exit.
    Raises subprocess.TimeoutExpired if the process exceeds *timeout_s*.
    subprocess.run() kills the child process before raising, so no cleanup needed.
    """
    try:
        result = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout_s,
        )
    except subprocess.TimeoutExpired:
        raise
    if result.returncode != 0:
        stderr = result.stderr.decode("utf-8", errors="replace")
        raise RuntimeError(f"Benchmark command failed: {command}\n{stderr}")

    if Constants.bench.startswith("tigon"):
        return _parse_tigon_output_to_csv(result.stdout, result.stderr, threads, seconds)
    return result.stdout


def _bench_timeout() -> float:
    """Return a generous per-run timeout in seconds.

    The benchmark is expected to finish in bench_n_seconds; we allow
    10× that plus a 15-second fixed overhead (for startup / teardown).
    """
    return max(30.0, Constants.bench_n_seconds * 10 + 15)


def run_experiment_lock_level_single_threaded():
    timeout = _bench_timeout()
    for i in range(Constants.n_program_iterations):
        for mutex_name in Constants.mutex_names:
            logger.info(f"{mutex_name=} | {i=}")
            data_file_name = get_data_file_name(mutex_name, i)
            if os.path.exists(data_file_name):
                os.remove(data_file_name)
            command = get_command(mutex_name, csv=True, thread_level=False)
            try:
                csv_data = _run_command_to_csv(
                    command, timeout,
                    threads=Constants.bench_n_threads,
                    seconds=Constants.bench_n_seconds,
                )
            except subprocess.TimeoutExpired:
                logger.warning(
                    f"HANG: {mutex_name} timed out after {timeout:.0f}s "
                    f"(rep={i}) — skipping this point"
                )
                continue
            with open(data_file_name, "wb") as data_file:
                data_file.write(csv_data)


def run_experiment_iter_single_threaded():
    timeout = _bench_timeout()
    for i in range(Constants.n_program_iterations):
        for iter_variable_value in range(*Constants.iter_range):
            # iter_kwargs names the sweep variable only (used for file names).
            # cmd_kwargs additionally includes rusage for the C++ argv.
            iter_kwargs = {Constants.iter_variable_name: iter_variable_value}
            cmd_kwargs  = {**iter_kwargs, "rusage": Constants.rusage}
            threads = iter_variable_value if Constants.iter_variable_name == "threads" else Constants.bench_n_threads
            for mutex_name in Constants.mutex_names:
                logger.info(f"{mutex_name=:<24} | {i=:0>2} | {cmd_kwargs=}")
                data_file_name = get_data_file_name(mutex_name, i, **iter_kwargs)
                if os.path.exists(data_file_name):
                    os.remove(data_file_name)
                command = get_command(mutex_name, csv=True, thread_level=Constants.thread_level, **cmd_kwargs)
                try:
                    csv_data = _run_command_to_csv(
                        command, timeout,
                        threads=threads,
                        seconds=Constants.bench_n_seconds,
                    )
                except subprocess.TimeoutExpired:
                    logger.warning(
                        f"HANG: {mutex_name} timed out after {timeout:.0f}s "
                        f"({Constants.iter_variable_name}={iter_variable_value}, rep={i}) "
                        f"— skipping this point"
                    )
                    continue
                with open(data_file_name, "wb") as data_file:
                    data_file.write(csv_data)