"""Legacy flag CLI: ``python -m scripts.main THREADS SECONDS REPS [options]``.

The flags are translated into an ExperimentConfig (scripts/config.py), so the
flag CLI and TOML experiment files share one schema and one validator.
"""

import argparse
import re

from .config import (
    PLACEMENTS,
    ConfigError,
    RunPaths,
    ExperimentConfig,
    LockSelection,
    Memory,
    Output,
    Plot,
    Sweep,
    Tigon,
    Workload,
    check_host,
    validate,
)
from .constants import Constants
from .logger import logger
from .settings import resolve_run


_ALLOC_ALIASES = {"malloc": "none", "cxl-hardware": "locks"}
_SWEEP_FLAG_VARS = ("threads", "critical-delay", "noncritical-delay")

# Config keys (as written in ConfigError messages) -> flag spellings.
_CLI_FLAGS = {
    "bench": "--bench",
    "capture": "--capture",
    "threads": "THREADS",
    "seconds": "SECONDS",
    "repetitions": "PROGRAM_ITERATIONS",
    "sweep": "--sweep",
    "sweep.var": "--sweep VAR",
    "sweep.start": "--sweep START",
    "sweep.stop": "--sweep STOP",
    "sweep.step": "--sweep STEP",
    "locks.sets": "--set",
    "locks.include": "--include",
    "locks.exclude": "--exclude",
    "memory.placement": "--placement",
    "memory.node": "--numa-node",
    "plot.layout": "--plot",
    "plot.show": "--show",
    "plot.scatter": "--scatter",
    "plot.max_points": "--max-n-points",
    "plot.variability": "--variability",
    "plot.speedup": "--speedup",
    "plot.heatmap_ref": "--heatmap-ref",
    "plot.stdev": "--stdev",
    "plot.averages": "--averages",
    "output.log_level": "--log",
}
_CLI_PHRASES = {"plot.heatmap = false": "--no-heatmap"}


def _cli_flag(key):
    if key in _CLI_FLAGS:
        return _CLI_FLAGS[key]
    section, _, name = key.partition(".")
    if section == "tigon":
        return "--tigon-" + name.replace("_", "-")
    if section == "workload":
        return "--" + name.replace("_", "-")
    return key


def render_for_cli(message):
    """Rewrite `config.key` references in a ConfigError message as flags."""
    def replace(match):
        text = match.group(1)
        if text in _CLI_PHRASES:
            return _CLI_PHRASES[text]
        key, sep, value = text.partition(" = ")
        key = key.strip()
        if not sep:
            return _cli_flag(key)
        if key == "sweep.var":
            return f"--sweep {value.replace('_', '-')}"
        return f"{_cli_flag(key)} {value}"
    return re.sub(r"`([^`]+)`", replace, message)


def _sweep_from_args(args):
    if args.sweep is None:
        return None
    variable_name, start_raw, stop_raw, step_raw = args.sweep
    if variable_name not in _SWEEP_FLAG_VARS:
        raise ConfigError(f"--sweep VAR must be one of {{{', '.join(_SWEEP_FLAG_VARS)}}}")
    try:
        start, stop, step = int(start_raw), int(stop_raw), int(step_raw)
    except ValueError as exc:
        raise ConfigError(f"--sweep expects integer START/STOP/STEP values: {exc}") from None
    return Sweep(var=variable_name.replace("-", "_"), start=start, stop=stop, step=step)


def config_from_args(args) -> ExperimentConfig:
    """Translate parsed legacy flags into a validated ExperimentConfig."""
    placement = args.placement
    if args.alloc is not None:
        alias = _ALLOC_ALIASES[args.alloc]
        if placement is not None and placement != alias:
            raise ConfigError(f"--alloc {args.alloc} conflicts with --placement {placement}")
        placement = alias

    cfg = ExperimentConfig(
        name=args.bench,
        bench=args.bench,
        capture=args.capture,
        threads=args.threads,
        seconds=float(args.seconds),
        repetitions=args.program_iterations,
        locks=LockSelection(
            sets=list(args.set or []),
            include=list(args.include or []),
            exclude=list(args.exclude or []),
        ),
        sweep=_sweep_from_args(args),
        memory=Memory(placement=placement or "none", node=args.numa_node),
        workload=Workload(
            groups=args.groups,
            critical_delay=args.critical_delay,
            noncritical_delay=args.noncritical_delay,
            stagger_ms=args.stagger_ms,
        ),
        tigon=Tigon(
            query=args.tigon_query,
            protocol=args.tigon_protocol,
            partitions=args.tigon_partitions,
            partition_ratio=args.tigon_partition_ratio,
            warmup_seconds=args.tigon_warmup_seconds,
            read_write_ratio=args.tigon_read_write_ratio,
            read_only_ratio=args.tigon_read_only_ratio,
            cross_ratio=args.tigon_cross_ratio,
            cross_partitions=args.tigon_cross_partitions,
            keys=args.tigon_keys,
            zipf=args.tigon_zipf,
            neworder_dist=args.tigon_neworder_dist,
            payment_dist=args.tigon_payment_dist,
        ),
        plot=Plot(
            layout=args.plot,
            show=args.show,
            scatter=args.scatter,
            max_points=args.max_n_points,
            variability=args.variability,
            speedup=args.speedup,
            heatmap=not args.no_heatmap,
            heatmap_ref=args.heatmap_ref,
            stdev=args.stdev,
            averages=args.averages,
        ),
        output=Output(log_level=args.log),
    )
    validate(cfg)
    return cfg


def build_parser():
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
        help="re-plot existing CSVs instead of running (default folder: the newest run under data/generated)",
    )
    capture_group.add_argument(
        "--data-folder",
        type=str,
        default=Constants.Defaults.DATA_FOLDER,
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
        default=Constants.Defaults.MAX_N_POINTS,
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
        "--placement",
        choices=PLACEMENTS,
        help=(
            "memory placement: none (default); process = run each benchmark under "
            "numactl --membind; locks = -Dhardware_cxl build, lock memory mbind()-placed"
        ),
    )
    selection_group.add_argument(
        "--alloc",
        choices=tuple(_ALLOC_ALIASES),
        help="deprecated alias: malloc = --placement none, cxl-hardware = --placement locks",
    )
    selection_group.add_argument(
        "--numa-node",
        type=int,
        metavar="N",
        help="NUMA node for --placement process/locks (required with them)",
    )
    selection_group.add_argument(
        "--log-folder",
        type=str,
        default=Constants.Defaults.LOGS_FOLDER,
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

    return parser


def init_args(argv=None):
    """Parse legacy flags, validate them, and resolve them into RunSettings.

    Returns the RunSettings (its config is run.cfg). Folders keep the flag
    CLI's layout: CSVs in --data-folder (a timestamped subfolder of the
    default), figures in ../figs, and config.toml + run.json beside the CSVs.
    """
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        cfg = config_from_args(args)
        if not args.reuse_data:
            check_host(cfg)
        paths = RunPaths.legacy(args.data_folder, args.log_folder, args.reuse_data)
    except ConfigError as exc:
        parser.error(render_for_cli(str(exc)))
    run = resolve_run(cfg, paths)
    logger.debug(run.data_folder)
    return run
