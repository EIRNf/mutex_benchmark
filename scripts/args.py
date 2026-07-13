import argparse
import logging

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


def _validate_context(args, parser):
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

    if args.capture == "rusage" and args.bench == "min":
        parser.error("--capture rusage is not supported by --bench min")

    if args.capture == "latency":
        if args.variability:
            parser.error("--variability only applies to --capture throughput sweeps")
        if args.speedup is not None:
            parser.error("--speedup only applies to --capture throughput sweeps")
        if args.stdev != Constants.Defaults.STANDARD_DEVIATION_SCALE:
            parser.error("--stdev only applies to --capture throughput sweeps")
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
        choices=("max", "min", "grouped"),
        default="max",
        help="benchmark executable to run",
    )
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
        choices=("malloc", "cxl-software", "cxl-hardware"),
        default="malloc",
        help="allocation mode for benchmark build/runtime",
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

    Constants.data_folder = args.data_folder
    logger.debug(Constants.data_folder)
    Constants.logs_folder = args.log_folder
    Constants.multithreaded = args.multithreaded
    Constants.bench = args.bench
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
    if args.plot == "faceted":
        Constants.faceted = True
    elif args.plot == "combined":
        Constants.faceted = False
    else:
        Constants.faceted = None

    Constants.software_cxl = args.alloc == "cxl-software"
    Constants.hardware_cxl = args.alloc == "cxl-hardware"

    level = getattr(logging, args.log.upper(), Constants.Defaults.LOG)
    Constants.log = level
    logger.setLevel(level)

    return args
