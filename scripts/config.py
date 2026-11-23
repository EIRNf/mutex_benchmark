"""Experiment configuration: one schema shared by TOML files and the flag CLI.

An ExperimentConfig describes a single experiment (one benchmark, one lock
list, one optional sweep, one memory placement) independent of where its
results go. It can be built three ways, all of which go through the same
validation:

  * load_config(path, overrides)      -- a TOML file plus ``-o key=value``
  * load_config(None, overrides)      -- defaults plus overrides only
  * scripts.args.config_from_args()   -- the legacy flag CLI

Where results go is a RunPaths, chosen by the entry point: a fresh run folder
(``RunPaths.for_new_run``), an existing one to re-plot
(``RunPaths.for_existing_run``), or the flag CLI's flat folders
(``RunPaths.legacy``). scripts/settings.py resolves a config and its paths
into the RunSettings that the builder, runner, dataloader and plotter take.

Error messages name settings by their config key in backticks (for example
`tigon.query`); the flag CLI rewrites those keys to flag names before printing.
"""

from __future__ import annotations

import copy
import math
import os
import re
import shutil
import sys
import tomllib
import types
import typing
from dataclasses import MISSING, dataclass, field, fields, is_dataclass
from datetime import datetime
from difflib import get_close_matches
from typing import Any

from .constants import Constants


BENCHES = ("max", "min", "grouped", "tigon", "tigon_ycsb", "tigon_tpcc")
CAPTURES = ("latency", "throughput", "rusage")
SWEEP_VARS = ("threads", "critical_ns", "noncritical_ns", "critical_delay", "noncritical_delay")
LEGACY_DELAYS = ("critical_delay", "noncritical_delay")   # busy-loop iterations, latency only
PIN_MODES = ("none", "compact", "scatter")
DEFAULT_WARMUP_SECONDS = 0.5
PLACEMENTS = ("none", "process", "locks")
PLOT_LAYOUTS = ("auto", "faceted", "combined", "none")
LOG_LEVELS = ("DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL")
TIGON_PROTOCOLS = (
    "Silo", "SiloGC", "Star", "Sundial", "TwoPL", "TwoPLGC", "Calvin",
    "HStore", "Aria", "TwoPLPasha", "SundialPasha",
)
TIGON_YCSB_QUERIES = ("mixed", "rmw", "scan", "insert", "delete")
TIGON_TPCC_QUERIES = ("mixed", "first_two", "neworder", "payment", "test")
TIGON_PARTITION_RATIOS = ("threads", "half", "quarter")
TIGON_YCSB_FIELDS = (
    "read_write_ratio", "read_only_ratio", "cross_ratio",
    "cross_partitions", "keys", "zipf",
)
TIGON_TPCC_FIELDS = ("neworder_dist", "payment_dist")


class ConfigError(ValueError):
    """An invalid experiment configuration; the message is user-facing."""


# --------------------------------------------------------------------------
# Schema. Fields typed ``X | None`` are optional: None means "not set" (TOML
# has no null, so leave the key out), and the documented default applies.
# --------------------------------------------------------------------------

@dataclass
class LockSelection:
    sets: list[str] = field(default_factory=list)      # named sets from constants.py
    include: list[str] = field(default_factory=list)   # extra lock names
    exclude: list[str] = field(default_factory=list)


@dataclass
class Sweep:
    var: str     # threads | critical_delay | noncritical_delay
    start: int
    stop: int    # inclusive
    step: int


@dataclass
class Memory:
    # none:    no placement; ordinary malloc build, OS default policy.
    # process: run each benchmark under `numactl --membind=node`, so every
    #          allocation (lock state, stacks, benchmark and Tigon data) is
    #          bound to the node.
    # locks:   build with -Dhardware_cxl; lock memory is mbind()-placed on the
    #          node via MUTEX_BENCH_NUMA_NODE.
    placement: str = "none"
    node: int | None = None


@dataclass
class Workload:
    groups: int | None = None             # grouped only (required there)
    # max only. With no critical_ns/noncritical_ns the run is the roof: each
    # thread reacquires the lock as soon as it releases it.
    critical_ns: int | None = None        # fixed work inside the critical section, ns
    noncritical_ns: int | None = None     # random work between acquisitions, mean ns (uniform 0-2x)
    warmup: float | None = None           # seconds run before measuring; new runs default to 0.5
    pin: str | None = None                # none | compact | scatter (Linux); new runs default to none
    critical_delay: int | None = None     # max, capture = latency only: legacy busy-loop iterations
    noncritical_delay: int | None = None  # max, capture = latency only: legacy busy-loop iterations
    stagger_ms: int | None = None         # max/min only


@dataclass
class Tigon:
    query: str | None = None              # default: rmw (YCSB) / mixed (TPC-C)
    protocol: str | None = None           # default: TwoPL
    partitions: int | None = None         # default: one per worker thread
    partition_ratio: str | None = None    # threads | half | quarter
    warmup_seconds: int | None = None     # default: min(1, seconds - 1)
    read_write_ratio: int | None = None   # YCSB, default 80
    read_only_ratio: int | None = None    # YCSB, default 0
    cross_ratio: int | None = None        # YCSB, default 0
    cross_partitions: int | None = None   # YCSB, default 2
    keys: int | None = None               # YCSB, default 200000
    zipf: float | None = None             # YCSB, default 0
    neworder_dist: int | None = None      # TPC-C, default 10
    payment_dist: int | None = None       # TPC-C, default 15


@dataclass
class Plot:
    layout: str = "auto"                  # auto | faceted | combined | none
    show: bool = False
    scatter: bool = False                 # latency CDFs only
    max_points: int = Constants.Defaults.MAX_N_POINTS  # latency CDFs only
    variability: bool = False
    speedup: str | None = None            # reference lock for a speedup table
    heatmap: bool = True
    heatmap_ref: str | None = None
    stdev: float = Constants.Defaults.STANDARD_DEVIATION_SCALE
    averages: bool = False


@dataclass
class Output:
    log_level: str = "INFO"


@dataclass
class ExperimentConfig:
    name: str = ""
    bench: str = "max"
    capture: str = "latency"
    threads: int = Constants.Defaults.BENCH_N_THREADS   # unused by a threads sweep
    seconds: float = float(Constants.Defaults.BENCH_N_SECONDS)
    repetitions: int = Constants.Defaults.N_PROGRAM_ITERATIONS
    locks: LockSelection = field(default_factory=LockSelection)
    sweep: Sweep | None = None
    memory: Memory = field(default_factory=Memory)
    workload: Workload = field(default_factory=Workload)
    tigon: Tigon = field(default_factory=Tigon)
    plot: Plot = field(default_factory=Plot)
    output: Output = field(default_factory=Output)

    @property
    def is_tigon(self) -> bool:
        return self.bench.startswith("tigon")


# --------------------------------------------------------------------------
# Where an experiment's results go.
# --------------------------------------------------------------------------

RUNS_ROOT = "./data/runs"
CONFIG_FILE = "config.toml"
RECORD_FILE = "run.json"
_TIMESTAMP_DIR = re.compile(r"^\d{8}T\d{6}\d*$")


def timestamp() -> str:
    return datetime.now().strftime("%Y%m%dT%H%M%S%f")


@dataclass
class RunPaths:
    data: str                    # CSVs
    plots: str                   # figures and speedup tables
    logs: str
    run_dir: str | None = None   # holds config.toml and run.json
    reuse: bool = False          # plot existing data instead of running

    @classmethod
    def for_new_run(cls, name: str, run_dir: str | None = None, root: str = RUNS_ROOT):
        """A fresh self-describing run folder: config.toml, run.json, raw/, plots/, logs/.

        A given run_dir may already hold a config.toml (a suite prepares its
        run folders that way) but not a run.json, which marks a started run.
        """
        if run_dir is None:
            run_dir = os.path.join(root, f"{timestamp()}-{name}")
        elif os.path.exists(os.path.join(run_dir, RECORD_FILE)):
            raise ConfigError(f"{run_dir} already holds a run; choose another directory")
        return cls(
            data=os.path.join(run_dir, "raw"),
            plots=os.path.join(run_dir, "plots"),
            logs=os.path.join(run_dir, "logs"),
            run_dir=run_dir,
        )

    @classmethod
    def for_existing_run(cls, run_dir: str):
        """Re-plot a recorded run (new layout, or a flag-CLI data folder with config.toml)."""
        if not os.path.isfile(os.path.join(run_dir, CONFIG_FILE)):
            raise ConfigError(f"{run_dir} has no {CONFIG_FILE}; it was not recorded as a run")
        if os.path.isdir(os.path.join(run_dir, "raw")):
            return cls(
                data=os.path.join(run_dir, "raw"),
                plots=os.path.join(run_dir, "plots"),
                logs=os.path.join(run_dir, "logs"),
                run_dir=run_dir,
                reuse=True,
            )
        return cls(
            data=run_dir,
            plots=os.path.normpath(os.path.join(run_dir, "..", "figs")),
            logs=Constants.Defaults.LOGS_FOLDER,
            run_dir=run_dir,
            reuse=True,
        )

    @classmethod
    def legacy(cls, data_folder: str, log_folder: str, reuse: bool,
               default_root: str = Constants.Defaults.DATA_FOLDER):
        """The flag CLI's layout: CSVs in data_folder, figures in data_folder/../figs.

        With the default data folder, new runs get a timestamped subfolder and
        --reuse-data picks the newest such subfolder that holds CSVs.
        """
        if data_folder == default_root:
            if reuse:
                data_folder = _newest_timestamped_run(default_root)
            else:
                data_folder = os.path.join(default_root, timestamp())
        return cls(
            data=data_folder,
            plots=os.path.normpath(os.path.join(data_folder, "..", "figs")),
            logs=log_folder,
            run_dir=None if reuse else data_folder,
            reuse=reuse,
        )


def _newest_timestamped_run(root: str) -> str:
    try:
        names = sorted((n for n in os.listdir(root) if _TIMESTAMP_DIR.match(n)), reverse=True)
    except OSError:
        names = []
    for name in names:
        candidate = os.path.join(root, name)
        if any(entry.endswith(".csv") for entry in os.listdir(candidate)):
            return candidate
    raise ConfigError(f"no previous runs with CSV data under {root}; pass the run's data folder")


# --------------------------------------------------------------------------
# Building a config from plain data (TOML tables, overrides).
# --------------------------------------------------------------------------

def _type_hints(cls) -> dict[str, Any]:
    return typing.get_type_hints(cls)


def _unwrap_optional(tp):
    """Return (inner_type, is_optional) for ``X | None``."""
    if isinstance(tp, types.UnionType) or typing.get_origin(tp) is typing.Union:
        args = [a for a in typing.get_args(tp) if a is not type(None)]
        if len(args) == 1:
            return args[0], True
    return tp, False


def _describe(tp) -> str:
    if typing.get_origin(tp) is list:
        return "a list of strings"
    return {int: "an integer", float: "a number", str: "a string", bool: "true or false"}.get(
        tp, getattr(tp, "__name__", str(tp))
    )


def _coerce(value, tp, path):
    inner, optional = _unwrap_optional(tp)
    if value is None:
        if optional:
            return None
        raise ConfigError(f"`{path}` is required")
    if is_dataclass(inner):
        if not isinstance(value, dict):
            raise ConfigError(f"`{path}` must be a table")
        return _from_dict(inner, value, path + ".")
    if inner is bool:
        ok = isinstance(value, bool)
    elif inner is int:
        ok = isinstance(value, int) and not isinstance(value, bool)
    elif inner is float:
        ok = isinstance(value, (int, float)) and not isinstance(value, bool)
        if ok:
            value = float(value)
    elif inner is str:
        ok = isinstance(value, str)
    elif typing.get_origin(inner) is list:
        ok = isinstance(value, list) and all(isinstance(v, str) for v in value)
        if ok:
            value = list(value)
    else:  # pragma: no cover - schema uses only the types above
        raise TypeError(f"unsupported schema type for {path}: {inner}")
    if not ok:
        raise ConfigError(f"`{path}` must be {_describe(inner)}; got {value!r}")
    return value


def _from_dict(cls, data: dict, prefix: str = ""):
    hints = _type_hints(cls)
    names = [f.name for f in fields(cls)]
    unknown = [key for key in data if key not in names]
    if unknown:
        key = unknown[0]
        where = f"`{prefix}{key}`" if prefix else f"`{key}`"
        hint = get_close_matches(key, names, n=1)
        suggestion = f"; did you mean `{prefix}{hint[0]}`?" if hint else ""
        raise ConfigError(f"unknown setting {where}{suggestion}")
    kwargs = {}
    for f in fields(cls):
        if f.name in data:
            kwargs[f.name] = _coerce(data[f.name], hints[f.name], f"{prefix}{f.name}")
    try:
        obj = cls(**kwargs)
    except TypeError:
        missing = [
            f.name for f in fields(cls)
            if f.name not in kwargs
            and f.default is MISSING
            and f.default_factory is MISSING
        ]
        table = prefix.rstrip(".") or "config"
        raise ConfigError(
            f"`{table}` is missing {', '.join(f'`{prefix}{m}`' for m in missing)}"
        ) from None
    if isinstance(obj, Sweep):
        obj.var = obj.var.replace("-", "_")
    return obj



def _field_type(path: list[str]):
    """Schema type at a dotted path, or raise ConfigError for unknown keys."""
    cls = ExperimentConfig
    tp = None
    for depth, key in enumerate(path):
        hints = _type_hints(cls)
        if key not in hints:
            names = list(hints)
            hint = get_close_matches(key, names, n=1)
            prefix = ".".join(path[:depth])
            full = f"{prefix}.{key}" if prefix else key
            suggestion = ""
            if hint:
                suggestion = f"; did you mean `{prefix + '.' if prefix else ''}{hint[0]}`?"
            raise ConfigError(f"unknown setting `{full}`{suggestion}")
        tp, _ = _unwrap_optional(hints[key])
        if depth < len(path) - 1:
            if not is_dataclass(tp):
                raise ConfigError(f"`{'.'.join(path[:depth + 1])}` is not a table")
            cls = tp
    return tp


def _parse_override_value(raw: str, tp):
    """Interpret an override's text according to the target field's type."""
    try:
        value = tomllib.loads(f"v = {raw}")["v"]
    except tomllib.TOMLDecodeError:
        value = raw  # bare word such as `bench=max`
    if typing.get_origin(tp) is list and isinstance(value, str):
        value = [item.strip() for item in value.split(",") if item.strip()]
    elif tp is str and not isinstance(value, str):
        value = raw  # e.g. `name=2026` stays a string
    return value


def set_dotted(data: dict, key: str, value) -> None:
    """Set one setting by dotted key in raw config data, creating tables as needed.

    The value is checked against the schema later, when the data becomes an
    ExperimentConfig; only the key is checked here.
    """
    path = key.split(".")
    if is_dataclass(_field_type(path)):
        raise ConfigError(f"`{key}` is a table; set one of its keys instead")
    table = data
    for part in path[:-1]:
        child = table.setdefault(part, {})
        if not isinstance(child, dict):
            raise ConfigError(f"`{part}` is not a table")
        table = child
    table[path[-1]] = value


def apply_overrides(data: dict, overrides) -> dict:
    """Apply ``key=value`` overrides (dotted keys) to raw config data in place."""
    for override in overrides or ():
        key, sep, raw = override.partition("=")
        key = key.strip()
        if not sep or not key:
            raise ConfigError(f"override {override!r} must look like key=value")
        set_dotted(data, key, _parse_override_value(raw.strip(), _field_type(key.split("."))))
    return data


def config_from_data(data: dict, overrides=(), *, name: str = "") -> ExperimentConfig:
    data = apply_overrides(copy.deepcopy(data), overrides)
    cfg = _from_dict(ExperimentConfig, data)
    if not cfg.name:
        cfg.name = name or cfg.bench
    validate(cfg)
    return cfg


def config_from_toml(text: str, overrides=(), *, name: str = "") -> ExperimentConfig:
    try:
        data = tomllib.loads(text)
    except tomllib.TOMLDecodeError as exc:
        raise ConfigError(f"invalid TOML: {exc}") from None
    return config_from_data(data, overrides, name=name)


def load_config(path: str | None, overrides=()) -> ExperimentConfig:
    """Load a TOML experiment file (or only defaults when path is None)."""
    if path is None:
        return config_from_data({}, overrides)
    try:
        with open(path, "rb") as handle:
            text = handle.read().decode("utf-8")
    except OSError as exc:
        raise ConfigError(f"cannot read config {path}: {exc.strerror}") from None
    stem = os.path.splitext(os.path.basename(path))[0]
    try:
        return config_from_toml(text, overrides, name=stem)
    except ConfigError as exc:
        raise ConfigError(f"{path}: {exc}") from None


# --------------------------------------------------------------------------
# Validation
# --------------------------------------------------------------------------

def _require(condition: bool, message: str):
    if not condition:
        raise ConfigError(message)


def _one_of(value, choices, path):
    _require(value in choices, f"`{path}` must be one of {', '.join(choices)}; got {value!r}")


def resolve_lock_names(locks: LockSelection) -> list[str]:
    """Expand sets/include/exclude into the ordered lock list to run."""
    if locks.sets:
        names = []
        for set_name in locks.sets:
            lock_set = Constants.Defaults.MUTEX_SETS.get(set_name)
            if lock_set is None:
                available = ", ".join(sorted(Constants.Defaults.MUTEX_SETS))
                raise ConfigError(f"unknown lock set {set_name!r} in `locks.sets`; available: {available}")
            names.extend(lock_set)
        names = list(dict.fromkeys(names))
    elif locks.include:
        names = list(locks.include)
    else:
        names = list(Constants.Defaults.MUTEX_NAMES)
    names = [name for name in names if name not in locks.exclude]
    for name in locks.include:
        if name not in names:
            names.append(name)
    return names


def _validate_tigon(cfg: ExperimentConfig):
    t = cfg.tigon
    set_fields = [f.name for f in fields(Tigon) if getattr(t, f.name) is not None]
    if not cfg.is_tigon:
        _require(not set_fields, f"`tigon.{set_fields[0] if set_fields else ''}` only applies to "
                                 "bench tigon/tigon_ycsb/tigon_tpcc")
        return
    _require(cfg.capture == "throughput", f"bench {cfg.bench} currently supports `capture` = throughput only")
    is_tpcc = cfg.bench == "tigon_tpcc"
    if t.query is not None:
        valid = TIGON_TPCC_QUERIES if is_tpcc else TIGON_YCSB_QUERIES
        _require(t.query in valid,
                 f"`tigon.query` {t.query!r} is not valid for bench {cfg.bench}; "
                 f"choose from {', '.join(sorted(valid))}")
    if t.protocol is not None:
        _one_of(t.protocol, TIGON_PROTOCOLS, "tigon.protocol")
    if t.partition_ratio is not None:
        _one_of(t.partition_ratio, TIGON_PARTITION_RATIOS, "tigon.partition_ratio")
    _require(t.partitions is None or t.partitions >= 1, "`tigon.partitions` must be >= 1")
    _require(t.partitions is None or t.partition_ratio is None,
             "`tigon.partitions` and `tigon.partition_ratio` cannot be used together")
    _require(t.warmup_seconds is None or t.warmup_seconds >= 0, "`tigon.warmup_seconds` must be >= 0")

    other = TIGON_YCSB_FIELDS if is_tpcc else TIGON_TPCC_FIELDS
    wrong = [name for name in other if getattr(t, name) is not None]
    _require(not wrong,
             f"`tigon.{wrong[0] if wrong else ''}` is {'YCSB' if is_tpcc else 'TPC-C'}-specific "
             f"and cannot be used with bench {cfg.bench}")
    percent_fields = TIGON_TPCC_FIELDS if is_tpcc else ("read_write_ratio", "read_only_ratio", "cross_ratio")
    for name in percent_fields:
        value = getattr(t, name)
        _require(value is None or 0 <= value <= 100, f"`tigon.{name}` must be between 0 and 100")
    if not is_tpcc:
        _require(t.cross_partitions is None or t.cross_partitions >= 1, "`tigon.cross_partitions` must be >= 1")
        _require(t.keys is None or t.keys >= 1, "`tigon.keys` must be >= 1")
        _require(t.zipf is None or (math.isfinite(t.zipf) and t.zipf >= 0),
                 "`tigon.zipf` must be a finite value >= 0")


def validate(cfg: ExperimentConfig):
    """Check a config for consistency. Host-independent; see check_host()."""
    _one_of(cfg.bench, BENCHES, "bench")
    _one_of(cfg.capture, CAPTURES, "capture")
    _one_of(cfg.memory.placement, PLACEMENTS, "memory.placement")
    _one_of(cfg.plot.layout, PLOT_LAYOUTS, "plot.layout")
    _require(cfg.output.log_level.upper() in LOG_LEVELS,
             f"`output.log_level` must be one of {', '.join(LOG_LEVELS)}; got {cfg.output.log_level!r}")
    _require(cfg.threads >= 1, "`threads` must be >= 1")
    _require(cfg.seconds > 0, "`seconds` must be > 0")
    _require(cfg.repetitions >= 1, "`repetitions` must be >= 1")
    resolve_lock_names(cfg.locks)  # rejects unknown sets

    sweep = cfg.sweep
    if sweep is not None:
        _one_of(sweep.var, SWEEP_VARS, "sweep.var")
        _require(sweep.step > 0, "`sweep.step` must be > 0")
        _require(sweep.stop >= sweep.start, "`sweep.stop` must be >= `sweep.start`")

    if cfg.memory.placement == "none":
        _require(cfg.memory.node is None,
                 "`memory.node` requires `memory.placement` to be process or locks")
    else:
        _require(cfg.memory.node is not None,
                 f"`memory.placement = {cfg.memory.placement}` requires `memory.node`")
        _require(cfg.memory.node >= 0, "`memory.node` must be >= 0")

    _validate_tigon(cfg)

    w = cfg.workload
    if cfg.bench == "grouped":
        _require(w.groups is not None and w.groups > 0,
                 "`workload.groups` must be set and > 0 for bench grouped")
        _require(sweep is not None, "bench grouped requires a `sweep`")
    else:
        _require(w.groups is None, "`workload.groups` only applies to bench grouped")
    _validate_max_workload(cfg)
    if w.stagger_ms is not None:
        _require(w.stagger_ms > 0, "`workload.stagger_ms` must be > 0")
        _require(cfg.bench in ("max", "min"), "`workload.stagger_ms` only applies to bench max/min")

    if cfg.capture in ("throughput", "rusage"):
        _require(sweep is not None, f"`capture = {cfg.capture}` requires a `sweep`")
    if cfg.capture == "rusage":
        _require(cfg.bench != "min" and not cfg.is_tigon,
                 f"`capture = rusage` is not supported by bench {cfg.bench}")

    _validate_plot(cfg)


def _validate_max_workload(cfg: ExperimentConfig):
    w, sweep = cfg.workload, cfg.sweep
    for name in ("critical_ns", "noncritical_ns", "warmup", "pin", *LEGACY_DELAYS):
        _require(getattr(w, name) is None or cfg.bench == "max",
                 f"`workload.{name}` only applies to bench max")
    for name in ("critical_ns", "noncritical_ns"):
        value = getattr(w, name)
        _require(value is None or value >= 0, f"`workload.{name}` must be >= 0 (nanoseconds)")
    for name in LEGACY_DELAYS:
        value = getattr(w, name)
        _require(value is None or value >= 0, f"`workload.{name}` must be >= 0")
    _require(w.warmup is None or (math.isfinite(w.warmup) and w.warmup >= 0),
             "`workload.warmup` must be >= 0 seconds")
    if w.pin is not None:
        _one_of(w.pin, PIN_MODES, "workload.pin")
    if sweep is not None and sweep.var != "threads":
        _require(cfg.bench == "max", f"`sweep.var = {sweep.var}` only applies to bench max")
        _require(getattr(w, sweep.var) is None,
                 f"set either `workload.{sweep.var}` or `sweep.var = {sweep.var}`, not both")
        _require(sweep.start >= 0, f"`sweep.start` must be >= 0 for `sweep.var = {sweep.var}`")

    swept = sweep.var if sweep is not None else None
    for name in LEGACY_DELAYS:
        ns_name = name.replace("_delay", "_ns")
        legacy_set = getattr(w, name) is not None or swept == name
        ns_set = getattr(w, ns_name) is not None or swept == ns_name
        _require(not (legacy_set and ns_set),
                 f"set either `workload.{ns_name}` or the legacy `workload.{name}`, not both")


def _validate_plot(cfg: ExperimentConfig):
    p = cfg.plot
    throughput_only = (
        (p.variability, "`plot.variability` only applies to `capture = throughput` sweeps"),
        (p.speedup is not None, "`plot.speedup` only applies to `capture = throughput` sweeps"),
        (p.stdev != Constants.Defaults.STANDARD_DEVIATION_SCALE,
         "`plot.stdev` only applies to `capture = throughput` sweeps"),
    )
    if cfg.capture != "throughput":
        for is_set, message in throughput_only:
            _require(not is_set, message)
    if cfg.capture == "latency":
        _require(p.heatmap_ref is None and p.heatmap,
                 "`plot.heatmap_ref`/`plot.heatmap = false` only apply to throughput sweep plots")
        _require(p.layout not in ("faceted", "combined"),
                 "`plot.layout = faceted/combined` only applies to throughput sweep plots")
        _require(cfg.sweep is None or p.layout == "none",
                 "`capture = latency` with a `sweep` currently requires `plot.layout = none`")
    else:
        _require(not p.scatter, "`plot.scatter` only applies to `capture = latency`")
        _require(p.max_points == Constants.Defaults.MAX_N_POINTS,
                 "`plot.max_points` only applies to `capture = latency`")
    if p.layout in ("faceted", "combined"):
        _require(cfg.sweep is not None and cfg.capture == "throughput",
                 "`plot.layout = faceted/combined` requires `capture = throughput` with a `sweep`")
    if p.layout == "none":
        _require(not p.show, "`plot.show` cannot be used with `plot.layout = none`")
        _require(not p.scatter, "`plot.scatter` has no effect with `plot.layout = none`")
        _require(p.max_points == Constants.Defaults.MAX_N_POINTS,
                 "`plot.max_points` has no effect with `plot.layout = none`")
        _require(p.heatmap_ref is None and p.heatmap,
                 "`plot.heatmap_ref`/`plot.heatmap = false` have no effect with `plot.layout = none`")
    _require(p.heatmap or p.heatmap_ref is None,
             "`plot.heatmap_ref` cannot be combined with `plot.heatmap = false`")


def check_host(cfg: ExperimentConfig):
    """Checks that need the machine the experiment will run on."""
    if cfg.workload.pin not in (None, "none"):
        _require(sys.platform.startswith("linux"), "`workload.pin` requires Linux")
        from .topology import TopologyError, host_cpus
        try:
            host_cpus()
        except TopologyError as exc:
            raise ConfigError(f"`workload.pin = {cfg.workload.pin}`: {exc}") from None
    placement = cfg.memory.placement
    if placement == "none":
        return
    node = cfg.memory.node
    _require(sys.platform.startswith("linux"), f"`memory.placement = {placement}` requires Linux")
    if placement == "process":
        _require(shutil.which("numactl") is not None,
                 "`memory.placement = process` requires numactl on PATH")
    meminfo_path = f"/sys/devices/system/node/node{node}/meminfo"
    memory_kib = None
    try:
        with open(meminfo_path, encoding="utf-8") as meminfo:
            for line in meminfo:
                parts = line.split()
                if parts[:3] == ["Node", str(node), "MemTotal:"]:
                    try:
                        memory_kib = int(parts[3])
                    except (IndexError, ValueError):
                        raise ConfigError(f"could not read memory size for NUMA node {node}") from None
                    break
    except OSError as exc:
        raise ConfigError(f"NUMA node {node} is not available: {exc}") from None
    _require(memory_kib is not None and memory_kib > 0, f"NUMA node {node} has no online memory")


# --------------------------------------------------------------------------
# Serialization (resolved config -> TOML text)
# --------------------------------------------------------------------------

def _toml_value(value) -> str:
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, (int, float)):
        return repr(value)
    if isinstance(value, str):
        return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'
    if isinstance(value, list):
        return "[" + ", ".join(_toml_value(v) for v in value) + "]"
    raise TypeError(f"cannot write {value!r} as TOML")


def to_toml(cfg: ExperimentConfig) -> str:
    """Write a config as TOML; unset (None) settings are omitted."""
    top, tables = [], []
    for f in fields(cfg):
        value = getattr(cfg, f.name)
        if value is None:
            continue
        if is_dataclass(value):
            lines = [
                f"{sub.name} = {_toml_value(getattr(value, sub.name))}"
                for sub in fields(value)
                if getattr(value, sub.name) is not None
            ]
            if lines:
                tables.append(f"[{f.name}]\n" + "\n".join(lines))
        else:
            top.append(f"{f.name} = {_toml_value(value)}")
    return "\n".join(top) + "\n\n" + "\n\n".join(tables) + "\n"


# --------------------------------------------------------------------------
# Derived values shared with scripts/settings.py and scripts/suite.py.
# --------------------------------------------------------------------------

def check_new_run(cfg: ExperimentConfig):
    """Rules for starting a run that recorded runs may break when re-plotted.

    The legacy delays (busy-loop iterations) only ever changed lock-level
    runs; throughput and rusage runs silently ignored them, so older run
    folders can hold such configs and still re-plot.
    """
    if cfg.capture == "latency":
        return
    swept = cfg.sweep.var if cfg.sweep is not None else None
    for name in LEGACY_DELAYS:
        ns_name = name.replace("_delay", "_ns")
        _require(getattr(cfg.workload, name) is None and swept != name,
                 f"`workload.{name}` (busy-loop iterations) only affects `capture = latency`; "
                 f"use `workload.{ns_name}` (nanoseconds) instead")


def with_run_defaults(cfg: ExperimentConfig) -> ExperimentConfig:
    """The config a new run records, with max's warmup and pinning made explicit.

    Run folders written before these settings existed have no such keys in
    their config.toml; leaving them unset there keeps a re-plot from claiming
    a warmup that never ran.
    """
    w = cfg.workload
    if cfg.bench != "max" or (w.warmup is not None and w.pin is not None):
        return cfg
    cfg = copy.deepcopy(cfg)
    if cfg.workload.warmup is None:
        cfg.workload.warmup = DEFAULT_WARMUP_SECONDS
    if cfg.workload.pin is None:
        cfg.workload.pin = "none"
    return cfg


def work_tag(critical_ns=None, noncritical_ns=None, pin=None, *, swept: dict | None = None) -> str:
    """Name fragment declaring a max run's added work and pinning ('' for the unpinned roof).

    critical_ns=200, noncritical_ns=1000, pin="compact" -> "cs200ns-ncs1000ns-pin-compact".
    swept maps a swept work variable to its (start, stop), e.g. {"critical_ns": (0, 2000)}
    -> "cs0-2000ns".
    """
    swept = swept or {}
    parts = []
    for prefix, name, value in (("cs", "critical_ns", critical_ns), ("ncs", "noncritical_ns", noncritical_ns)):
        if name in swept:
            start, stop = swept[name]
            parts.append(f"{prefix}{start}-{stop}ns")
        elif value:
            parts.append(f"{prefix}{value}ns")
    if pin and pin != "none":
        parts.append(f"pin-{pin}")
    return "-".join(parts)


def run_work_tag(cfg: ExperimentConfig, *, with_sweep: bool = True) -> str:
    """work_tag() for a config; with_sweep=False leaves out a swept work variable."""
    if cfg.bench != "max":
        return ""
    swept = {}
    if with_sweep and cfg.sweep is not None and cfg.sweep.var in ("critical_ns", "noncritical_ns"):
        swept[cfg.sweep.var] = (cfg.sweep.start, cfg.sweep.stop)
    w = cfg.workload
    return work_tag(w.critical_ns, w.noncritical_ns, w.pin, swept=swept)


def run_title(cfg: ExperimentConfig) -> str:
    """The run's name with its added work declared (the name of its run folder)."""
    tag = run_work_tag(cfg)
    if not tag or cfg.name.endswith(tag):
        return cfg.name
    return f"{cfg.name}-{tag}"


def tigon_warmup_seconds(cfg_seconds: float, warmup: int | None) -> int:
    run_seconds = max(1, int(cfg_seconds))
    return warmup if warmup is not None else min(1, run_seconds - 1)
