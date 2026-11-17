import argparse
import logging
import math
import os
import sys
from datetime import datetime

from .constants import Constants
from .logger import logger


def _ordered_dedupe(values):
    return list(dict.fromkeys(values))


def _resolve_mutex_names(args, parser):
    if args.set:
        mutex_names = []
        for set_name in args.set:
            mutex_set = Constants.Defaults.MUTEX_SETS.get(set_name)
            if mutex_set is None:
                available_sets = ", ".join(sorted(Constants.Defaults.MUTEX_SETS.keys()))
                parser.error(f"Unknown mutex set '{set_name}'. Available sets: {available_sets}")
            mutex_names.extend(mutex_set)
        mutex_names = _ordered_dedupe(mutex_names)
    elif args.include:
        mutex_names = list(args.include)
    else:
        mutex_names = [
            name for name in Constants.Defaults.MUTEX_NAMES
            if name not in args.exclude
        ]

    if args.exclude:
        for excluded_name in args.exclude:
            if excluded_name in mutex_names:
                mutex_names.remove(excluded_name)

    if args.include:
        for included_name in args.include:
            if included_name not in mutex_names:
                mutex_names.append(included_name)

    return mutex_names


def _resolve_executable_for_bench(bench_name):
    executable = Constants.Defaults.BENCH_EXECUTABLES.get(bench_name)
    if executable is None:
        raise NotImplementedError(f"Unknown executable: {bench_name}")
    return executable


def _parse_sweep_config(args, parser):
    if args.sweep is None:
        Constants.iter = False
        return

    variable_name, start_raw, stop_raw, step_raw = args.sweep
    try:
        start = int(start_raw)
        stop = int(stop_raw)
        step = int(step_raw)
    except ValueError as exc:
        parser.error(f"--sweep expects integer START/STOP/STEP values: {exc}")

    if step <= 0:
        parser.error("--sweep STEP must be > 0")
    if stop < start:
        parser.error("--sweep STOP must be >= START")

    Constants.iter_variable_name = variable_name.replace("-", "_")
    Constants.iter_range = [start, stop + 1, step]  # end-inclusive CLI
    Constants.iter = True


def _validate_numa_node(args, parser):
    if args.alloc != "cxl-hardware":
        if args.numa_node is not None:
            parser.error("--numa-node only applies to --alloc cxl-hardware")
        return

    if args.numa_node is None:
        parser.error("--numa-node is required when --alloc cxl-hardware")
    if args.numa_node < 0:
        parser.error("--numa-node must be >= 0")
    if not sys.platform.startswith("linux"):
        parser.error("--alloc cxl-hardware requires Linux")

    meminfo_path = f"/sys/devices/system/node/node{args.numa_node}/meminfo"
    memory_kib = None
    try:
        with open(meminfo_path, encoding="utf-8") as meminfo:
            for line in meminfo:
                fields = line.split()
                if fields[:3] == ["Node", str(args.numa_node), "MemTotal:"]:
                    try:
                        memory_kib = int(fields[3])
                    except (IndexError, ValueError):
                        parser.error(f"Could not read memory size for NUMA node {args.numa_node}")
                    break
    except OSError as exc:
        parser.error(f"NUMA node {args.numa_node} is not available: {exc}")

    if memory_kib is None or memory_kib <= 0:
        parser.error(f"NUMA node {args.numa_node} has no online memory")


def _validate_context(args, parser):
    _validate_numa_node(args, parser)

    tigon_option_names = (
        "tigon_query",
        "tigon_protocol",
        "tigon_partitions",
        "tigon_partition_ratio",
        "tigon_warmup_seconds",
        "tigon_read_write_ratio",
        "tigon_read_only_ratio",
        "tigon_cross_ratio",
        "tigon_cross_partitions",
        "tigon_keys",
        "tigon_zipf",
        "tigon_neworder_dist",
        "tigon_payment_dist",
    )
    is_tigon = args.bench.startswith("tigon")
    if not is_tigon and any(getattr(args, name) is not None for name in tigon_option_names):
        parser.error("--tigon-* options only apply to --bench tigon/tigon_ycsb/tigon_tpcc")

    if is_tigon:
        is_tpcc = args.bench == "tigon_tpcc"
        valid_queries = (
            {"mixed", "first_two", "neworder", "payment", "test"}
            if is_tpcc
            else {"mixed", "rmw", "scan", "insert", "delete"}
        )
        if args.tigon_query is not None and args.tigon_query not in valid_queries:
            parser.error(
                f"--tigon-query {args.tigon_query!r} is not valid for --bench {args.bench}; "
                f"choose from {', '.join(sorted(valid_queries))}"
            )
        if args.tigon_partitions is not None and args.tigon_partitions < 1:
            parser.error("--tigon-partitions must be >= 1")
        if args.tigon_partitions is not None and args.tigon_partition_ratio is not None:
            parser.error("--tigon-partitions and --tigon-partition-ratio cannot be used together")
        if args.tigon_warmup_seconds is not None and args.tigon_warmup_seconds < 0:
            parser.error("--tigon-warmup-seconds must be >= 0")

        ycsb_option_names = (
            "tigon_read_write_ratio",
            "tigon_read_only_ratio",
            "tigon_cross_ratio",
            "tigon_cross_partitions",
            "tigon_keys",
            "tigon_zipf",
        )
        tpcc_option_names = ("tigon_neworder_dist", "tigon_payment_dist")
        workload_only_names = tpcc_option_names if is_tpcc else ycsb_option_names
        irrelevant_names = ycsb_option_names if is_tpcc else tpcc_option_names
        if any(getattr(args, name) is not None for name in irrelevant_names):
            parser.error(
                f"{'YCSB' if is_tpcc else 'TPC-C'}-specific --tigon-* options "
                f"cannot be used with --bench {args.bench}"
            )
        if is_tpcc:
            for name in workload_only_names:
                value = getattr(args, name)
                if value is not None and not 0 <= value <= 100:
                    parser.error(f"--{name.replace('_', '-')} must be between 0 and 100")
        else:
            for name in ("tigon_read_write_ratio", "tigon_read_only_ratio", "tigon_cross_ratio"):
                value = getattr(args, name)
                if value is not None and not 0 <= value <= 100:
                    parser.error(f"--{name.replace('_', '-')} must be between 0 and 100")
            if args.tigon_cross_partitions is not None and args.tigon_cross_partitions < 1:
                parser.error("--tigon-cross-partitions must be >= 1")
            if args.tigon_keys is not None and args.tigon_keys < 1:
                parser.error("--tigon-keys must be >= 1")
            if args.tigon_zipf is not None and (
                not math.isfinite(args.tigon_zipf) or args.tigon_zipf < 0
            ):
                parser.error("--tigon-zipf must be a finite value >= 0")

    if args.bench == "grouped":
        if args.groups is None or args.groups <= 0:
            parser.error("--groups must be provided and > 0 when --bench grouped")
        if args.sweep is None:
            parser.error("--bench grouped requires --sweep")
    elif args.groups is not None:
        parser.error("--groups only applies to --bench grouped")

    if args.critical_delay is not None and args.bench != "max":
        parser.error("--critical-delay only applies to --bench max")
    if args.noncritical_delay is not None and args.bench != "max":
        parser.error("--noncritical-delay only applies to --bench max")

    if args.sweep is not None and args.sweep[0] in ("critical-delay", "noncritical-delay") and args.bench != "max":
        parser.error("--sweep critical-delay/noncritical-delay only applies to --bench max")

    if args.capture in ("throughput", "rusage") and args.sweep is None:
        parser.error("--capture throughput/rusage requires --sweep")

    if args.capture == "rusage" and (args.bench == "min" or args.bench.startswith("tigon")):
        parser.error(f"--capture rusage is not supported by --bench {args.bench}")

    if args.bench.startswith("tigon") and args.capture != "throughput":
        parser.error(f"--bench {args.bench} currently supports --capture throughput")

    if args.capture == "latency":
        if args.variability:
            parser.error("--variability only applies to --capture throughput sweeps")
        if args.speedup is not None:
            parser.error("--speedup only applies to --capture throughput sweeps")
        if args.stdev != Constants.Defaults.STANDARD_DEVIATION_SCALE:
            parser.error("--stdev only applies to --capture throughput sweeps")
        if args.heatmap_ref is not None or args.no_heatmap:
            parser.error("--heatmap-ref/--no-heatmap only apply to throughput sweep plots")
        if args.plot in ("faceted", "combined"):
            parser.error("--plot faceted/combined only applies to throughput sweep plots")
        if args.sweep is not None and args.plot != "none":
            parser.error("--capture latency with --sweep currently requires --plot none")
    else:
        if args.scatter:
            parser.error("--scatter only applies to --capture latency")
        if args.max_n_points != Constants.Defaults.MAX_N_POINTS:
            parser.error("--max-n-points only applies to --capture latency")

    if args.capture != "throughput":
        if args.variability:
            parser.error("--variability only applies to --capture throughput sweeps")
        if args.speedup is not None:
            parser.error("--speedup only applies to --capture throughput sweeps")
        if args.stdev != Constants.Defaults.STANDARD_DEVIATION_SCALE:
            parser.error("--stdev only applies to --capture throughput sweeps")

    if args.plot in ("faceted", "combined"):
        if args.sweep is None or args.capture != "throughput":
            parser.error("--plot faceted/combined requires --capture throughput with --sweep")

    if args.show and args.plot == "none":
        parser.error("--show cannot be used with --plot none")
    if args.plot == "none":
        if args.scatter:
            parser.error("--scatter has no effect when --plot none")
        if args.max_n_points != Constants.Defaults.MAX_N_POINTS:
            parser.error("--max-n-points has no effect when --plot none")
        if args.heatmap_ref is not None or args.no_heatmap:
            parser.error("--heatmap-ref/--no-heatmap have no effect when --plot none")
    if args.no_heatmap and args.heatmap_ref is not None:
        parser.error("--heatmap-ref cannot be used with --no-heatmap")

    if args.stagger_ms is not None:
        if args.stagger_ms <= 0:
            parser.error("--stagger-ms must be > 0")
        if args.bench not in ("max", "min"):
            parser.error("--stagger-ms only applies to --bench max/min")

    if args.critical_delay is not None and args.sweep is not None and args.sweep[0] == "critical-delay":
        parser.error("Use either --critical-delay or --sweep critical-delay, not both")
    if args.noncritical_delay is not None and args.sweep is not None and args.sweep[0] == "noncritical-delay":
        parser.error("Use either --noncritical-delay or --sweep noncritical-delay, not both")


def init_args():
    parser = argparse.ArgumentParser(
        prog="MutexTest",
        description="Run contention benchmarks on various mutex algorithms",
    )

    parser.add_argument(
        "threads",
        type=int,
        help="number of threads in contention",
    )
    parser.add_argument("seconds", type=float, help="run duration in seconds")
    parser.add_argument(
        "program_iterations",
        type=int,
        help="number of times to run each benchmark point",
    )

    run_group = parser.add_argument_group("run options (what executes)")
    run_group.add_argument(
        "--bench",
        choices=("max", "min", "grouped", "tigon", "tigon_ycsb", "tigon_tpcc"),
        default="max",
        help="benchmark executable to run",
    )
    run_group.add_argument(
        "--tigon-query",
        choices=("mixed", "rmw", "scan", "insert", "delete", "first_two", "neworder", "payment", "test"),
        help="Tigon query mix (YCSB: rmw/mixed/scan/insert/delete; TPC-C: mixed/first_two/neworder/payment/test)",
    )
    run_group.add_argument(
        "--tigon-protocol",
        choices=("Silo", "SiloGC", "Star", "Sundial", "TwoPL", "TwoPLGC", "Calvin", "HStore", "Aria", "TwoPLPasha", "SundialPasha"),
        help="Tigon transaction protocol (default: TwoPL)",
    )
    run_group.add_argument(
        "--tigon-partitions",
        type=int,
        help="fixed Tigon database partition count (default: match the worker thread count)",
    )
    run_group.add_argument(
        "--tigon-partition-ratio",
        choices=("threads", "half", "quarter"),
        help="derive partition count per point from the thread count (default: threads)",
    )
    run_group.add_argument(
        "--tigon-warmup-seconds",
        type=int,
        help="Tigon warmup duration (default: min(1, run-seconds - 1))",
    )
    run_group.add_argument("--tigon-read-write-ratio", type=int, help="YCSB read/write ratio, 0-100 (default: 80)")
    run_group.add_argument("--tigon-read-only-ratio", type=int, help="YCSB read-only transaction ratio, 0-100 (default: 0)")
    run_group.add_argument("--tigon-cross-ratio", type=int, help="YCSB cross-partition transaction ratio, 0-100 (default: 0)")
    run_group.add_argument("--tigon-cross-partitions", type=int, help="YCSB cross-partition count (default: 2)")
    run_group.add_argument("--tigon-keys", type=int, help="YCSB key count per partition (default: 200000)")
    run_group.add_argument("--tigon-zipf", type=float, help="YCSB Zipf skew (default: 0, uniform)")
    run_group.add_argument("--tigon-neworder-dist", type=int, help="TPC-C new-order cross-partition probability, 0-100 (default: 10)")
    run_group.add_argument("--tigon-payment-dist", type=int, help="TPC-C payment cross-partition probability, 0-100 (default: 15)")
    run_group.add_argument(
        "--sweep",
        nargs=4,
        metavar=("VAR", "START", "STOP", "STEP"),
        help=(
            "sweep one variable over an inclusive range; "
            "VAR is one of {threads, critical-delay, noncritical-delay}"
        ),
    )
    run_group.add_argument("--groups", type=int, help="number of groups for --bench grouped")
    run_group.add_argument("--critical-delay", type=int, help="critical section busy delay (max bench only)")
    run_group.add_argument("--noncritical-delay", type=int, help="non-critical section busy delay (max bench only)")
    run_group.add_argument("--stagger-ms", type=int, help="milliseconds between thread startups (max/min only)")
    run_group.add_argument("-m", "--multithreaded", action="store_true", help="spawn bench processes in parallel")

    capture_group = parser.add_argument_group("capture options (what is recorded)")
    capture_group.add_argument(
        "--capture",
        choices=("latency", "throughput", "rusage"),
        default="latency",
        help="measurement mode for captured data",
    )
    capture_group.add_argument(
        "--reuse-data",
        action="store_true",
        default=False,
        help="reuse previous CSV files instead of rerunning benchmarks",
    )
    capture_group.add_argument(
        "--data-folder",
        type=str,
        default=Constants.data_folder,
        help="where to write/read CSV output",
    )

    graph_group = parser.add_argument_group("graph options (how output is shown)")
    graph_group.add_argument(
        "--plot",
        choices=("auto", "faceted", "combined", "none"),
        default="auto",
        help="plot layout mode",
    )
    graph_group.add_argument(
        "--show",
        action="store_true",
        help="display plots interactively (in addition to saving them)",
    )
    graph_group.add_argument("--scatter", action="store_true", help="scatter CDF plots instead of lines")
    graph_group.add_argument(
        "-p",
        "--max-n-points",
        type=int,
        default=Constants.max_n_points,
        help="max points to sample on CDF plots",
    )
    graph_group.add_argument(
        "--variability",
        action="store_true",
        default=False,
        help="plot coefficient of variation (throughput sweep only)",
    )
    graph_group.add_argument(
        "--speedup",
        nargs="?",
        const="exp_spin",
        default=None,
        metavar="REF_MUTEX",
        help="print speedup table relative to REF_MUTEX (throughput sweep only; default exp_spin)",
    )
    graph_group.add_argument(
        "--heatmap-ref",
        metavar="REF_MUTEX",
        help="heatmap reference lock (default: --speedup reference, otherwise exp_spin)",
    )
    graph_group.add_argument(
        "--no-heatmap",
        action="store_true",
        help="omit the default relative-throughput heatmap",
    )
    graph_group.add_argument(
        "--stdev",
        type=float,
        default=Constants.Defaults.STANDARD_DEVIATION_SCALE,
        help="standard deviation scaling for throughput sweep analytics",
    )
    graph_group.add_argument("--averages", action="store_true", help="print lock-level averages")

    selection_group = parser.add_argument_group("selection/infra options")
    selection_group.add_argument("-s", "--set", nargs="+", help="run mutex comparison set(s)")
    selection_group.add_argument("-i", "--include", nargs="+", help="include these mutex names")
    selection_group.add_argument("-x", "--exclude", nargs="+", default=[], help="exclude these mutex names")
    selection_group.add_argument(
        "--alloc",
        choices=("malloc", "cxl-hardware"),
        default="malloc",
        help="allocation mode for benchmark build/runtime",
    )
    selection_group.add_argument(
        "--numa-node",
        type=int,
        metavar="N",
        help="memory NUMA node for --alloc cxl-hardware (required)",
    )
    selection_group.add_argument(
        "--log-folder",
        type=str,
        default=Constants.logs_folder,
        help="where to write debug logs",
    )

    log_group = parser.add_mutually_exclusive_group()
    log_group.add_argument(
        "-l",
        "--log",
        type=str,
        default="INFO",
        help="console log level (DEBUG, INFO, WARNING, ERROR, CRITICAL)",
    )
    log_group.add_argument("-d", "--debug", action="store_const", const="DEBUG", dest="log", help="set log level DEBUG")

    args = parser.parse_args()

    if args.sweep is not None and args.sweep[0] not in ("threads", "critical-delay", "noncritical-delay"):
        parser.error("--sweep VAR must be one of {threads, critical-delay, noncritical-delay}")

    _validate_context(args, parser)
    _parse_sweep_config(args, parser)

    Constants.mutex_names = _resolve_mutex_names(args, parser)
    Constants.bench_n_threads = args.threads
    Constants.bench_n_seconds = args.seconds
    Constants.n_program_iterations = args.program_iterations
    Constants.averages = args.averages

    Constants.capture = args.capture
    Constants.rusage = args.capture == "rusage"
    Constants.thread_level = args.capture in ("throughput", "rusage")

    data_folder = args.data_folder
    if not args.reuse_data and data_folder == Constants.Defaults.DATA_FOLDER:
        run_timestamp = datetime.now().strftime("%Y%m%dT%H%M%S%f")
        data_folder = os.path.join(data_folder, run_timestamp)
    Constants.data_folder = data_folder
    logger.debug(Constants.data_folder)
    Constants.logs_folder = args.log_folder
    Constants.multithreaded = args.multithreaded
    Constants.bench = args.bench
    is_tigon = args.bench.startswith("tigon")
    is_tigon_tpcc = args.bench == "tigon_tpcc"
    Constants.tigon_query = args.tigon_query or ("mixed" if is_tigon_tpcc else "rmw")
    Constants.tigon_protocol = args.tigon_protocol or "TwoPL"
    Constants.tigon_partitions = args.tigon_partitions
    Constants.tigon_partition_ratio = args.tigon_partition_ratio
    Constants.tigon_warmup_seconds = args.tigon_warmup_seconds
    Constants.tigon_read_write_ratio = (
        args.tigon_read_write_ratio if args.tigon_read_write_ratio is not None else 80
    )
    Constants.tigon_read_only_ratio = (
        args.tigon_read_only_ratio if args.tigon_read_only_ratio is not None else 0
    )
    Constants.tigon_cross_ratio = args.tigon_cross_ratio if args.tigon_cross_ratio is not None else 0
    Constants.tigon_cross_partitions = (
        args.tigon_cross_partitions if args.tigon_cross_partitions is not None else 2
    )
    Constants.tigon_keys = args.tigon_keys if args.tigon_keys is not None else 200000
    Constants.tigon_zipf = args.tigon_zipf if args.tigon_zipf is not None else 0.0
    Constants.tigon_neworder_dist = (
        args.tigon_neworder_dist if args.tigon_neworder_dist is not None else 10
    )
    Constants.tigon_payment_dist = (
        args.tigon_payment_dist if args.tigon_payment_dist is not None else 15
    )
    Constants.bench_variant = ""
    if is_tigon:
        run_seconds = max(1, int(Constants.bench_n_seconds))
        warmup_seconds = (
            Constants.tigon_warmup_seconds
            if Constants.tigon_warmup_seconds is not None
            else min(1, run_seconds - 1)
        )
        partition_tag = (
            f"ratio-{Constants.tigon_partition_ratio}"
            if Constants.tigon_partition_ratio is not None
            else str(Constants.tigon_partitions)
            if Constants.tigon_partitions is not None
            else "auto"
        )
        variant_parts = [
            f"query-{Constants.tigon_query}",
            f"protocol-{Constants.tigon_protocol}",
            (
                f"partition-{partition_tag}"
                if partition_tag.startswith("ratio-")
                else f"partitions-{partition_tag}"
            ),
            f"run-{run_seconds}s",
            f"warmup-{warmup_seconds}s",
        ]
        if is_tigon_tpcc:
            variant_parts.extend([
                f"neworder-{Constants.tigon_neworder_dist}",
                f"payment-{Constants.tigon_payment_dist}",
            ])
        else:
            variant_parts.extend([
                f"rw-{Constants.tigon_read_write_ratio}",
                f"ro-{Constants.tigon_read_only_ratio}",
                f"cross-{Constants.tigon_cross_ratio}",
                f"crossparts-{Constants.tigon_cross_partitions}",
                f"keys-{Constants.tigon_keys}",
                f"zipf-{Constants.tigon_zipf:g}",
            ])
        Constants.bench_variant = "-" + "-".join(variant_parts)
    Constants.groups = args.groups
    Constants.stdev_scale = args.stdev
    Constants.executable = _resolve_executable_for_bench(args.bench)

    Constants.scatter = args.scatter
    Constants.max_n_points = args.max_n_points
    Constants.critical_delay = args.critical_delay if args.critical_delay is not None else -1
    Constants.noncritical_delay = args.noncritical_delay if args.noncritical_delay is not None else -1
    Constants.skip_experiment = args.reuse_data

    Constants.stagger_ms = args.stagger_ms if args.stagger_ms is not None else 0
    Constants.low_contention = Constants.stagger_ms > 0

    Constants.plot = args.plot
    Constants.skip_plotting = args.plot == "none"
    Constants.show = args.show
    Constants.variability = args.variability
    Constants.speedup = args.speedup is not None
    Constants.speedup_ref = args.speedup
    Constants.heatmap_ref = args.heatmap_ref
    Constants.no_heatmap = args.no_heatmap
    if args.plot == "faceted":
        Constants.faceted = True
    elif args.plot == "combined":
        Constants.faceted = False
    else:
        Constants.faceted = None

    Constants.hardware_cxl = args.alloc == "cxl-hardware"
    Constants.numa_node = args.numa_node

    level = getattr(logging, args.log.upper(), Constants.Defaults.LOG)
    Constants.log = level
    logger.setLevel(level)

    return args
