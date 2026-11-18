# scripts/runner.py

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


class BenchmarkFailed(RuntimeError):
    """One benchmark execution exited non-zero or was killed by a signal.

    The message holds the command and both output streams; ``status`` is the
    short form ("exit code 1", "terminated by SIGSEGV (11)").
    """

    def __init__(self, message: str, status: str, stderr: bytes = b""):
        super().__init__(message)
        self.status = status
        lines = stderr.decode("utf-8", errors="replace").strip().splitlines()
        self.stderr_tail = lines[-3:]


def _format_return_code(returncode: int) -> str:
    if returncode >= 0:
        return f"exit code {returncode}"
    signal_number = -returncode
    try:
        signal_name = signal.Signals(signal_number).name
    except ValueError:
        signal_name = f"signal {signal_number}"
    return f"terminated by {signal_name} ({signal_number})"


def _is_default_tigon_workload(run) -> bool:
    if not run.bench.startswith("tigon"):
        return False
    common_defaults = (
        run.tigon_protocol == "TwoPL"
        and run.tigon_partitions is None
        and run.tigon_partition_ratio is None
        and run.tigon_warmup_seconds is None
    )
    if run.bench == "tigon_tpcc":
        return (
            common_defaults
            and run.tigon_query == "mixed"
            and run.tigon_neworder_dist == 10
            and run.tigon_payment_dist == 15
        )
    return (
        common_defaults
        and run.tigon_query == "rmw"
        and run.tigon_read_write_ratio == 80
        and run.tigon_read_only_ratio == 0
        and run.tigon_cross_ratio == 0
        and run.tigon_cross_partitions == 2
        and run.tigon_keys == 200000
        and run.tigon_zipf == 0.0
    )


def placement_tag(run) -> str:
    """Name fragment recording memory placement ('' when there is none)."""
    placement = run.placement
    if placement == "locks":
        return f"cxl-hw-node{run.numa_node}"
    if placement == "process":
        return f"membind-node{run.numa_node}"
    return ""


def _placement_prefix(run) -> list[str]:
    """Command prefix for whole-process placement."""
    if run.placement == "process":
        return ["numactl", f"--membind={run.numa_node}"]
    return []


def executable_path(run) -> str:
    """The configured benchmark binary inside the configured build directory."""
    return os.path.join(run.build_dir, run.executable)


def get_data_file_name(run, mutex_name, i, **iter_kwargs):
    """Return the CSV path for one benchmark point.

    iter_kwargs should contain only the swept variable (e.g. threads=4) —
    not the rusage flag; that is encoded in the capture segment instead.
    """
    capture = run.capture
    alloc_tag = placement_tag(run)
    if alloc_tag:
        alloc_tag = "-" + alloc_tag

    bench_tag = run.bench
    if run.bench.startswith("tigon"):
        variant = run.bench_variant
        if run.tigon_partitions is None:
            threads = iter_kwargs.get("threads", run.bench_n_threads)
            variant = variant.replace("partitions-auto", f"partitions-{threads}", 1)
        bench_tag += variant
    name_root = (
        f"{run.data_folder}/"
        f"{mutex_name}-{bench_tag}-{capture}{alloc_tag}-rep{i}"
    )
    for key, value in iter_kwargs.items():
        name_root += f"-{key}={value}"
    data_file_name = name_root + ".csv"

    if (
        run.skip_experiment
        and run.bench.startswith("tigon")
        and _is_default_tigon_workload(run)
        and not os.path.exists(data_file_name)
    ):
        legacy_root = (
            f"{run.data_folder}/"
            f"{mutex_name}-{run.bench}-{capture}{alloc_tag}-rep{i}"
        )
        for key, value in iter_kwargs.items():
            legacy_root += f"-{key}={value}"
        legacy_data_file_name = legacy_root + ".csv"
        if os.path.exists(legacy_data_file_name):
            return legacy_data_file_name
    return data_file_name

def get_command(run, mutex_name, *, threads=None, csv=True, thread_level=False, critical_delay=None, noncritical_delay=None, rusage=False):
    if threads is None:
        threads = run.bench_n_threads
    if critical_delay is None:
        critical_delay = run.critical_delay
    if noncritical_delay is None:
        noncritical_delay = run.noncritical_delay

    if run.bench.startswith("tigon"):
        run_sec = max(1, int(run.bench_n_seconds))
        warmup_sec = (
            run.tigon_warmup_seconds
            if run.tigon_warmup_seconds is not None
            else min(1, run_sec - 1)
        )
        if run.tigon_partitions is not None:
            partitions = run.tigon_partitions
        else:
            partition_ratio = run.tigon_partition_ratio or "threads"
            ratio_denominator = {"threads": 1, "half": 2, "quarter": 4}[partition_ratio]
            partitions = max(1, threads // ratio_denominator)
        is_tpcc = run.bench == "tigon_tpcc"
        app_name = "TPC-C" if is_tpcc else "YCSB"
        # Use with_lock.sh to inject the mutex via LD_PRELOAD
        cmd = [
            "./scripts/with_lock.sh",
            "-b", run.build_dir,
            "-n", str(max(128, threads * 4)),
            "-s", "app",
            mutex_name,
            executable_path(run),
            "--logtostderr=1",
            "--id=0",
            "--servers=127.0.0.1:1234",
            f"--threads={threads}",
            f"--partition_num={partitions}",
            f"--time_to_run={run_sec}",
            f"--time_to_warmup={warmup_sec}",
            f"--protocol={run.tigon_protocol}",
            f"--query={run.tigon_query}",
        ]
        run_details = [
            f"lock={mutex_name}",
            f"app={app_name}",
            f"query={run.tigon_query}",
            f"protocol={run.tigon_protocol}",
            "coordinator_id=0",
            "servers=127.0.0.1:1234",
            f"threads={threads}",
            f"partitions={partitions}",
            f"run_seconds={run_sec}",
            f"warmup_seconds={warmup_sec}",
        ]
        if run.tigon_partition_ratio is not None:
            run_details.append(f"partition_ratio={run.tigon_partition_ratio}")
        if is_tpcc:
            cmd.extend([
                f"--neworder_dist={run.tigon_neworder_dist}",
                f"--payment_dist={run.tigon_payment_dist}",
            ])
            run_details.extend([
                f"neworder_dist={run.tigon_neworder_dist}",
                f"payment_dist={run.tigon_payment_dist}",
            ])
        else:
            cmd.extend([
                f"--read_write_ratio={run.tigon_read_write_ratio}",
                f"--read_only_ratio={run.tigon_read_only_ratio}",
                f"--cross_ratio={run.tigon_cross_ratio}",
                f"--cross_part_num={run.tigon_cross_partitions}",
                f"--keys={run.tigon_keys}",
                f"--zipf={run.tigon_zipf:g}",
            ])
            run_details.extend([
                f"read_write_ratio={run.tigon_read_write_ratio}",
                f"read_only_ratio={run.tigon_read_only_ratio}",
                f"cross_ratio={run.tigon_cross_ratio}",
                f"cross_partitions={run.tigon_cross_partitions}",
                f"keys_per_partition={run.tigon_keys}",
                f"zipf={run.tigon_zipf:g}",
            ])
        if run.placement != "none":
            run_details.append(f"placement={run.placement}:node{run.numa_node}")
        cmd = _placement_prefix(run) + cmd
        logger.info("Tigon run configuration: %s", " ".join(run_details))
        logger.debug(f"Bench command: {cmd}")
        return cmd

    cmd = [
        executable_path(run),
        mutex_name,
        str(threads), 
        str(run.bench_n_seconds), 
    ]
    if run.groups:
        cmd.append(str(run.groups))

    # Only max_contention_bench simulates critical/noncritical-section delay;
    # min_/grouped_contention_bench don't recognize these flags and will
    # error out if passed.
    if run.bench == 'max':
        if critical_delay != -1:
            cmd += ["--critical-delay", str(critical_delay)]
        if noncritical_delay != -1:
            cmd += ["--noncritical-delay", str(noncritical_delay)]

    if csv:
        cmd.append("--csv")
    if thread_level:
        cmd.append("--thread-level")

    # min_contention_bench doesn't support --rusage.
    if rusage and run.bench in ('max', 'grouped'):
        cmd.append("--rusage")

    # grouped_contention_bench doesn't support staggered/low-contention startup.
    if run.low_contention and run.bench in ('max', 'min'):
        cmd.append("--low-contention")
        if run.stagger_ms and run.stagger_ms > 0:
            cmd += ["--stagger-ms", str(run.stagger_ms)]

    cmd = _placement_prefix(run) + cmd
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


def _run_command_to_csv(run, command, timeout_s: float, threads: int = 1, seconds: float = 1.0):
    """Run *command* and return its stdout bytes.

    Raises BenchmarkFailed on non-zero exit.
    Raises subprocess.TimeoutExpired if the process exceeds *timeout_s*.
    subprocess.run() kills the child process before raising, so no cleanup needed.
    """
    try:
        run_env = None
        if run.hardware_cxl:
            if run.numa_node is None:
                raise RuntimeError("--numa-node is required for hardware-CXL runs")
            run_env = os.environ.copy()
            run_env["MUTEX_BENCH_NUMA_NODE"] = str(run.numa_node)

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
        raise BenchmarkFailed(
            f"Benchmark command failed ({status}): {command}\n"
            f"--- stdout ---\n{stdout}\n"
            f"--- stderr ---\n{stderr}",
            status,
            result.stderr,
        )

    if run.bench.startswith("tigon"):
        return _parse_tigon_output_to_csv(result.stdout, result.stderr, threads, seconds)
    return result.stdout


def _bench_timeout(run) -> float:
    """Return a generous per-run timeout in seconds.

    Allow 10× the configured run duration (plus Tigon warmup) and a 15-second
    fixed overhead for startup and teardown.
    """
    expected_seconds = run.bench_n_seconds
    if run.bench.startswith("tigon"):
        run_sec = max(1, int(run.bench_n_seconds))
        warmup_sec = (
            run.tigon_warmup_seconds
            if run.tigon_warmup_seconds is not None
            else min(1, run_sec - 1)
        )
        expected_seconds = run_sec + warmup_sec
    return max(30.0, expected_seconds * 10 + 15)


class _PointTally:
    """What a run's benchmark executions produced, for its final check."""

    def __init__(self):
        self.written = 0
        self.problems: list[str] = []

    def check(self):
        """A run where no execution wrote data failed as a whole."""
        if self.problems and not self.written:
            raise RuntimeError(
                f"none of the {len(self.problems)} benchmark executions wrote data "
                f"(each failed or hung); first: {self.problems[0]}"
            )


def _save_failure_output(run, data_file_name: str, error: BenchmarkFailed) -> str | None:
    """Keep a failed execution's full output beside the run's logs."""
    name = os.path.splitext(os.path.basename(data_file_name))[0] + ".failed.txt"
    path = os.path.join(run.logs_folder, name)
    try:
        with open(path, "w", encoding="utf-8") as handle:
            handle.write(str(error) + "\n")
    except OSError:
        return None
    return path


def _run_point(run, mutex_name, command, data_file_name, timeout, tally, *, threads, where):
    """Run one benchmark execution into its CSV.

    A hang or a failed execution is logged and skipped, so one bad point
    does not cost the rest of the run.
    """
    try:
        csv_data = _run_command_to_csv(
            run,
            command, timeout,
            threads=threads,
            seconds=run.bench_n_seconds,
        )
    except subprocess.TimeoutExpired:
        logger.warning(
            f"HANG: {mutex_name} timed out after {timeout:.0f}s "
            f"({where}) — skipping this point"
        )
        tally.problems.append(f"{mutex_name} ({where}) timed out after {timeout:.0f}s")
        return
    except BenchmarkFailed as error:
        saved = _save_failure_output(run, data_file_name, error)
        logger.warning(
            f"FAILED: {mutex_name} {error.status} ({where}) — skipping this point"
            + (f"; full output in {saved}" if saved else "")
        )
        for line in error.stderr_tail:
            logger.warning(f"    {line}")
        tally.problems.append(f"{mutex_name} ({where}) {error.status}")
        return
    with open(data_file_name, "wb") as data_file:
        data_file.write(csv_data)
    tally.written += 1


def run_experiment_lock_level_single_threaded(run):
    timeout = _bench_timeout(run)
    tally = _PointTally()
    for i in range(run.n_program_iterations):
        for mutex_name in run.mutex_names:
            logger.info(f"{mutex_name=} | {i=}")
            data_file_name = get_data_file_name(run, mutex_name, i)
            if os.path.exists(data_file_name):
                os.remove(data_file_name)
            command = get_command(run, mutex_name, csv=True, thread_level=False)
            _run_point(run, mutex_name, command, data_file_name, timeout, tally,
                       threads=run.bench_n_threads, where=f"rep={i}")
    tally.check()


def run_experiment_iter_single_threaded(run):
    timeout = _bench_timeout(run)
    tally = _PointTally()
    for i in range(run.n_program_iterations):
        for iter_variable_value in range(*run.iter_range):
            # iter_kwargs names the sweep variable only (used for file names).
            # cmd_kwargs additionally includes rusage for the C++ argv.
            iter_kwargs = {run.iter_variable_name: iter_variable_value}
            cmd_kwargs  = {**iter_kwargs, "rusage": run.rusage}
            threads = iter_variable_value if run.iter_variable_name == "threads" else run.bench_n_threads
            for mutex_name in run.mutex_names:
                logger.info(f"{mutex_name=:<24} | {i=:0>2} | {cmd_kwargs=}")
                data_file_name = get_data_file_name(run, mutex_name, i, **iter_kwargs)
                if os.path.exists(data_file_name):
                    os.remove(data_file_name)
                command = get_command(run, mutex_name, csv=True, thread_level=run.thread_level, **cmd_kwargs)
                _run_point(run, mutex_name, command, data_file_name, timeout, tally,
                           threads=threads,
                           where=f"{run.iter_variable_name}={iter_variable_value}, rep={i}")
    tally.check()
