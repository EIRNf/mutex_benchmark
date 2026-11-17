# scripts/runner.py

from .constants import Constants
from .logger    import logger
import subprocess
import os
import re
import signal


def _format_command_output(output: bytes, *, limit: int = 80) -> str:
    text = output.decode("utf-8", errors="replace")
    lines = text.splitlines()
    if not lines:
        return "<empty>"
    if len(lines) > limit:
        omitted = len(lines) - limit
        lines = [f"... {omitted} earlier lines omitted ...", *lines[-limit:]]
    return "\n".join(lines)


def _format_return_code(returncode: int) -> str:
    if returncode >= 0:
        return f"exit code {returncode}"
    signal_number = -returncode
    try:
        signal_name = signal.Signals(signal_number).name
    except ValueError:
        signal_name = f"signal {signal_number}"
    return f"terminated by {signal_name} ({signal_number})"


def _is_default_tigon_workload() -> bool:
    if not Constants.bench.startswith("tigon"):
        return False
    common_defaults = (
        Constants.tigon_protocol == "TwoPL"
        and Constants.tigon_partitions is None
        and Constants.tigon_partition_ratio is None
        and Constants.tigon_warmup_seconds is None
    )
    if Constants.bench == "tigon_tpcc":
        return (
            common_defaults
            and Constants.tigon_query == "mixed"
            and Constants.tigon_neworder_dist == 10
            and Constants.tigon_payment_dist == 15
        )
    return (
        common_defaults
        and Constants.tigon_query == "rmw"
        and Constants.tigon_read_write_ratio == 80
        and Constants.tigon_read_only_ratio == 0
        and Constants.tigon_cross_ratio == 0
        and Constants.tigon_cross_partitions == 2
        and Constants.tigon_keys == 200000
        and Constants.tigon_zipf == 0.0
    )


def get_data_file_name(mutex_name, i, **iter_kwargs):
    """Return the CSV path for one benchmark point.

    iter_kwargs should contain only the swept variable (e.g. threads=4) —
    not the rusage flag; that is encoded in the capture segment instead.
    """
    capture = getattr(Constants, "capture", "latency")
    alloc_tag = ""
    if Constants.hardware_cxl:
        alloc_tag = f"-cxl-hw-node{Constants.numa_node}"
    elif Constants.software_cxl:
        alloc_tag = "-cxl-sw"

    bench_tag = Constants.bench
    if Constants.bench.startswith("tigon"):
        variant = Constants.bench_variant
        if Constants.tigon_partitions is None:
            threads = iter_kwargs.get("threads", Constants.bench_n_threads)
            variant = variant.replace("partitions-auto", f"partitions-{threads}", 1)
        bench_tag += variant
    name_root = (
        f"{Constants.data_folder}/"
        f"{mutex_name}-{bench_tag}-{capture}{alloc_tag}-rep{i}"
    )
    for key, value in iter_kwargs.items():
        name_root += f"-{key}={value}"
    data_file_name = name_root + ".csv"

    if (
        Constants.skip_experiment
        and Constants.bench.startswith("tigon")
        and _is_default_tigon_workload()
        and not os.path.exists(data_file_name)
    ):
        legacy_root = (
            f"{Constants.data_folder}/"
            f"{mutex_name}-{Constants.bench}-{capture}{alloc_tag}-rep{i}"
        )
        for key, value in iter_kwargs.items():
            legacy_root += f"-{key}={value}"
        legacy_data_file_name = legacy_root + ".csv"
        if os.path.exists(legacy_data_file_name):
            return legacy_data_file_name
    return data_file_name

def get_command(mutex_name, *, threads=None, csv=True, thread_level=False, critical_delay=None, noncritical_delay=None, rusage=False):
    if threads is None:
        threads = Constants.bench_n_threads
    if critical_delay is None:
        critical_delay = Constants.critical_delay
    if noncritical_delay is None:
        noncritical_delay = Constants.noncritical_delay

    if Constants.bench.startswith("tigon"):
        run_sec = max(1, int(Constants.bench_n_seconds))
        warmup_sec = (
            Constants.tigon_warmup_seconds
            if Constants.tigon_warmup_seconds is not None
            else min(1, run_sec - 1)
        )
        if Constants.tigon_partitions is not None:
            partitions = Constants.tigon_partitions
        else:
            partition_ratio = Constants.tigon_partition_ratio or "threads"
            ratio_denominator = {"threads": 1, "half": 2, "quarter": 4}[partition_ratio]
            partitions = max(1, threads // ratio_denominator)
        is_tpcc = Constants.bench == "tigon_tpcc"
        app_name = "TPC-C" if is_tpcc else "YCSB"
        # Use with_lock.sh to inject the mutex via LD_PRELOAD
        cmd = [
            "./scripts/with_lock.sh",
            "-n", str(max(128, threads * 4)),
            "-s", "app",
            mutex_name,
            Constants.executable,
            "--logtostderr=1",
            "--id=0",
            "--servers=127.0.0.1:1234",
            f"--threads={threads}",
            f"--partition_num={partitions}",
            f"--time_to_run={run_sec}",
            f"--time_to_warmup={warmup_sec}",
            f"--protocol={Constants.tigon_protocol}",
            f"--query={Constants.tigon_query}",
        ]
        run_details = [
            f"lock={mutex_name}",
            f"app={app_name}",
            f"query={Constants.tigon_query}",
            f"protocol={Constants.tigon_protocol}",
            "coordinator_id=0",
            "servers=127.0.0.1:1234",
            f"threads={threads}",
            f"partitions={partitions}",
            f"run_seconds={run_sec}",
            f"warmup_seconds={warmup_sec}",
        ]
        if Constants.tigon_partition_ratio is not None:
            run_details.append(f"partition_ratio={Constants.tigon_partition_ratio}")
        if is_tpcc:
            cmd.extend([
                f"--neworder_dist={Constants.tigon_neworder_dist}",
                f"--payment_dist={Constants.tigon_payment_dist}",
            ])
            run_details.extend([
                f"neworder_dist={Constants.tigon_neworder_dist}",
                f"payment_dist={Constants.tigon_payment_dist}",
            ])
        else:
            cmd.extend([
                f"--read_write_ratio={Constants.tigon_read_write_ratio}",
                f"--read_only_ratio={Constants.tigon_read_only_ratio}",
                f"--cross_ratio={Constants.tigon_cross_ratio}",
                f"--cross_part_num={Constants.tigon_cross_partitions}",
                f"--keys={Constants.tigon_keys}",
                f"--zipf={Constants.tigon_zipf:g}",
            ])
            run_details.extend([
                f"read_write_ratio={Constants.tigon_read_write_ratio}",
                f"read_only_ratio={Constants.tigon_read_only_ratio}",
                f"cross_ratio={Constants.tigon_cross_ratio}",
                f"cross_partitions={Constants.tigon_cross_partitions}",
                f"keys_per_partition={Constants.tigon_keys}",
                f"zipf={Constants.tigon_zipf:g}",
            ])
        logger.info("Tigon run configuration: %s", " ".join(run_details))
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
        run_env = None
        if Constants.hardware_cxl:
            if Constants.numa_node is None:
                raise RuntimeError("--numa-node is required for hardware-CXL runs")
            run_env = os.environ.copy()
            run_env["MUTEX_BENCH_NUMA_NODE"] = str(Constants.numa_node)

        result = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout_s,
            env=run_env,
        )
    except subprocess.TimeoutExpired:
        raise
    if os.environ.get("MUTEX_BENCH_PLACEMENT") == "1":
        for line in result.stderr.decode("utf-8", errors="replace").splitlines():
            if line.startswith("PLACEMENT "):
                logger.info(line)
    if result.returncode != 0:
        stdout = _format_command_output(result.stdout)
        stderr = _format_command_output(result.stderr)
        status = _format_return_code(result.returncode)
        raise RuntimeError(
            f"Benchmark command failed ({status}): {command}\n"
            f"--- stdout ---\n{stdout}\n"
            f"--- stderr ---\n{stderr}"
        )

    if Constants.bench.startswith("tigon"):
        return _parse_tigon_output_to_csv(result.stdout, result.stderr, threads, seconds)
    return result.stdout


def _bench_timeout() -> float:
    """Return a generous per-run timeout in seconds.

    Allow 10× the configured run duration (plus Tigon warmup) and a 15-second
    fixed overhead for startup and teardown.
    """
    expected_seconds = Constants.bench_n_seconds
    if Constants.bench.startswith("tigon"):
        run_sec = max(1, int(Constants.bench_n_seconds))
        warmup_sec = (
            Constants.tigon_warmup_seconds
            if Constants.tigon_warmup_seconds is not None
            else min(1, run_sec - 1)
        )
        expected_seconds = run_sec + warmup_sec
    return max(30.0, expected_seconds * 10 + 15)


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