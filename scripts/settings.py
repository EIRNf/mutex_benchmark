"""RunSettings: everything one run needs, resolved from its config and paths.

The builder, runner, data loader, plotter and run record all take a
RunSettings argument instead of reading shared global state, so several runs
can be prepared, inspected or loaded in one process (scripts/compare.py loads
every run of a suite this way). It is immutable; use dataclasses.replace() to
derive a variant.

Field names follow the attributes the pipeline historically read from the
global Constants object, so each `Constants.x` became `run.x`.
"""

from __future__ import annotations

import logging
import os
from dataclasses import dataclass

from .builder import default_build_dir
from .config import ExperimentConfig, RunPaths, resolve_lock_names, tigon_warmup_seconds
from .constants import Constants

Defaults = Constants.Defaults


@dataclass(frozen=True)
class RunSettings:
    cfg: ExperimentConfig
    experiment_name: str
    mutex_names: list[str]
    bench: str
    executable: str                 # relative to build_dir
    bench_n_threads: int
    bench_n_seconds: float
    n_program_iterations: int

    capture: str                    # latency | throughput | rusage
    rusage: bool
    thread_level: bool
    iter: bool                      # has a sweep
    iter_variable_name: str | None  # the swept variable, None without a sweep
    iter_range: list[int] | None    # [start, stop + 1, step]

    data_folder: str
    figs_folder: str
    logs_folder: str
    run_dir: str | None             # holds config.toml and run.json
    skip_experiment: bool           # plot existing data instead of running

    groups: int | None
    critical_delay: int             # -1 = not set
    noncritical_delay: int          # -1 = not set
    stagger_ms: int
    low_contention: bool

    tigon_query: str
    tigon_protocol: str
    tigon_partitions: int | None
    tigon_partition_ratio: str | None
    tigon_warmup_seconds: int | None
    tigon_read_write_ratio: int
    tigon_read_only_ratio: int
    tigon_cross_ratio: int
    tigon_cross_partitions: int
    tigon_keys: int
    tigon_zipf: float
    tigon_neworder_dist: int
    tigon_payment_dist: int
    bench_variant: str              # Tigon workload fragment of file names, "" otherwise

    plot: str
    skip_plotting: bool
    faceted: bool | None            # None = choose by series count
    show: bool
    scatter: bool
    max_n_points: int
    variability: bool
    speedup: bool
    speedup_ref: str | None
    heatmap_ref: str | None
    no_heatmap: bool
    stdev_scale: float
    averages: bool
    log_scale: bool

    placement: str                  # none | process | locks
    hardware_cxl: bool              # placement == "locks": -Dhardware_cxl build
    numa_node: int | None
    build_dir: str
    skip_build: bool                # reuse build_dir as is (still verified)

    log: int                        # console log level


def resolve_run(cfg: ExperimentConfig, paths: RunPaths | None = None, *,
                build_dir: str | None = None, skip_build: bool | None = None) -> RunSettings:
    """Resolve a validated config, its output paths and build choice for one run.

    build_dir defaults to the placement's own build folder (build/ or
    build-hardware_cxl/). skip_build defaults to MUTEX_BENCH_SKIP_BUILD=1.
    """
    if paths is None:
        paths = RunPaths.legacy(Defaults.DATA_FOLDER, Defaults.LOGS_FOLDER, False)
    if skip_build is None:
        skip_build = os.environ.get("MUTEX_BENCH_SKIP_BUILD") == "1"
    hardware_cxl = cfg.memory.placement == "locks"
    w, t, p = cfg.workload, cfg.tigon, cfg.plot
    is_tpcc = cfg.bench == "tigon_tpcc"
    stagger_ms = w.stagger_ms if w.stagger_ms is not None else 0

    tigon = dict(
        tigon_query=t.query or ("mixed" if is_tpcc else "rmw"),
        tigon_protocol=t.protocol or "TwoPL",
        tigon_partitions=t.partitions,
        tigon_partition_ratio=t.partition_ratio,
        tigon_warmup_seconds=t.warmup_seconds,
        tigon_read_write_ratio=t.read_write_ratio if t.read_write_ratio is not None else 80,
        tigon_read_only_ratio=t.read_only_ratio if t.read_only_ratio is not None else 0,
        tigon_cross_ratio=t.cross_ratio if t.cross_ratio is not None else 0,
        tigon_cross_partitions=t.cross_partitions if t.cross_partitions is not None else 2,
        tigon_keys=t.keys if t.keys is not None else 200000,
        tigon_zipf=t.zipf if t.zipf is not None else 0.0,
        tigon_neworder_dist=t.neworder_dist if t.neworder_dist is not None else 10,
        tigon_payment_dist=t.payment_dist if t.payment_dist is not None else 15,
    )

    return RunSettings(
        cfg=cfg,
        experiment_name=cfg.name,
        mutex_names=resolve_lock_names(cfg.locks),
        bench=cfg.bench,
        executable=Defaults.BENCH_EXECUTABLES[cfg.bench],
        bench_n_threads=cfg.threads,
        bench_n_seconds=cfg.seconds,
        n_program_iterations=cfg.repetitions,
        capture=cfg.capture,
        rusage=cfg.capture == "rusage",
        thread_level=cfg.capture in ("throughput", "rusage"),
        iter=cfg.sweep is not None,
        iter_variable_name=cfg.sweep.var if cfg.sweep else None,
        iter_range=[cfg.sweep.start, cfg.sweep.stop + 1, cfg.sweep.step] if cfg.sweep else None,
        data_folder=paths.data,
        figs_folder=paths.plots,
        logs_folder=paths.logs,
        run_dir=paths.run_dir,
        skip_experiment=paths.reuse,
        groups=w.groups,
        critical_delay=w.critical_delay if w.critical_delay is not None else -1,
        noncritical_delay=w.noncritical_delay if w.noncritical_delay is not None else -1,
        stagger_ms=stagger_ms,
        low_contention=stagger_ms > 0,
        **tigon,
        bench_variant=_tigon_variant(cfg, tigon) if cfg.is_tigon else "",
        plot=p.layout,
        skip_plotting=p.layout == "none",
        faceted={"faceted": True, "combined": False}.get(p.layout),
        show=p.show,
        scatter=p.scatter,
        max_n_points=p.max_points,
        variability=p.variability,
        speedup=p.speedup is not None,
        speedup_ref=p.speedup,
        heatmap_ref=p.heatmap_ref,
        no_heatmap=not p.heatmap,
        stdev_scale=p.stdev,
        averages=p.averages,
        log_scale=Defaults.LOG_SCALE,
        placement=cfg.memory.placement,
        hardware_cxl=hardware_cxl,
        numa_node=cfg.memory.node,
        build_dir=build_dir or default_build_dir(hardware_cxl),
        skip_build=skip_build,
        log=getattr(logging, cfg.output.log_level.upper()),
    )


def _tigon_variant(cfg: ExperimentConfig, tigon: dict) -> str:
    """The Tigon workload fragment of data file names."""
    run_seconds = max(1, int(cfg.seconds))
    warmup = tigon_warmup_seconds(cfg.seconds, tigon["tigon_warmup_seconds"])
    if tigon["tigon_partition_ratio"] is not None:
        partition_part = f"partition-ratio-{tigon['tigon_partition_ratio']}"
    elif tigon["tigon_partitions"] is not None:
        partition_part = f"partitions-{tigon['tigon_partitions']}"
    else:
        partition_part = "partitions-auto"
    parts = [
        f"query-{tigon['tigon_query']}",
        f"protocol-{tigon['tigon_protocol']}",
        partition_part,
        f"run-{run_seconds}s",
        f"warmup-{warmup}s",
    ]
    if cfg.bench == "tigon_tpcc":
        parts += [f"neworder-{tigon['tigon_neworder_dist']}", f"payment-{tigon['tigon_payment_dist']}"]
    else:
        parts += [
            f"rw-{tigon['tigon_read_write_ratio']}",
            f"ro-{tigon['tigon_read_only_ratio']}",
            f"cross-{tigon['tigon_cross_ratio']}",
            f"crossparts-{tigon['tigon_cross_partitions']}",
            f"keys-{tigon['tigon_keys']}",
            f"zipf-{tigon['tigon_zipf']:g}",
        ]
    return "-" + "-".join(parts)
