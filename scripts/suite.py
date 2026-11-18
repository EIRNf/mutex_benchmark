"""Experiment suites: several distinct experiments run as one unit.

A suite file (TOML) lists experiments and, optionally, the environments to
run each of them in:

    name = "placement"                 # default: the file name
    cgroup = "/sys/fs/cgroup/a/cgroup.procs"   # optional: run everything in this cgroup

    [defaults]                         # shared settings; same keys as an experiment file
    capture = "throughput"
    sweep = { var = "threads", start = 1, stop = 41, step = 4 }

    [[environments]]                   # optional; every experiment runs in each one
    name = "cxl-high"
    memory = { placement = "process", node = 1 }
    latency = "ffff"                   # CXL latency register value; needs [latency_control]

    [latency_control]
    pci = "0000:a0:00.0"               # the CXL device's PCI address on this host
    register = "0xe08.l"               # default

    [[experiments]]
    bench = "max"                      # name defaults to the bench (or the file's name)

    [[experiments]]
    file = "../tigon_ycsb.toml"        # start from an experiment file, relative to this one
    vary = { "tigon.partition_ratio" = ["threads", "half", "quarter"] }
    environments = ["cxl-high"]        # optional: run only in these environments

Each run's settings are layered: defaults < file < the entry's own keys <
vary < the environment's memory < command-line overrides. ``vary`` expands
one entry into one experiment per value (per combination for several keys).

Running a suite creates data/suites/<timestamp>-<name>/ with suite.toml (the
file as given), suite.json (the plan and each run's status) and one prepared
run folder per run at <environment>/<experiment>/. It builds each needed
build folder once, then runs every experiment as its own
``python -m scripts run`` process, so a failed or crashed run does not take
the others down. A suite folder can be resumed: complete runs are skipped,
and an unfinished run's folder is moved aside to <run>.attempt-N first.
"""

from __future__ import annotations

import copy
import fnmatch
import itertools
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tomllib
from dataclasses import dataclass, field, fields
from difflib import get_close_matches

from .builder import build, configured_cpp_args, default_build_dir, required_cpp_args
from .config import (
    CONFIG_FILE,
    ConfigError,
    ExperimentConfig,
    check_host,
    config_from_data,
    load_config,
    resolve_lock_names,
    set_dotted,
    tigon_warmup_seconds,
    timestamp,
    to_toml,
)
from .rundir import (
    SUITE_CONTEXT_ENV,
    command_line,
    git_info,
    host_info,
    read_json,
    read_record,
    utc_now,
    write_json_atomic,
)

SUITES_ROOT = "./data/suites"
SUITE_FILE = "suite.toml"
SUITE_RECORD = "suite.json"
DEFAULT_LATENCY_REGISTER = "0xe08.l"

_SUITE_KEYS = ("name", "cgroup", "defaults", "environments", "experiments", "latency_control")
_ENVIRONMENT_KEYS = ("name", "memory", "latency")
_LATENCY_CONTROL_KEYS = ("pci", "register")
_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._+=-]*$")
_BDF = re.compile(r"^([0-9A-Fa-f]{4}:)?[0-9A-Fa-f]{2}:[0-9A-Fa-f]{2}\.[0-7]$")
_REGISTER = re.compile(r"^(0x)?[0-9A-Fa-f]+\.[bwl]$")
_HEX_VALUE = re.compile(r"^(?:0x)?([0-9A-Fa-f]{1,8})$")
_CHILD_EXIT_GRACE_SECONDS = 30


# --------------------------------------------------------------------------
# The plan: what a suite runs.
# --------------------------------------------------------------------------

@dataclass
class LatencyControlSpec:
    pci: str
    register: str = DEFAULT_LATENCY_REGISTER


@dataclass
class Environment:
    name: str
    memory: dict | None = None    # raw [memory] table, validated with each run
    latency: str | None = None    # hex register value without 0x; None = host setting


@dataclass
class PlannedRun:
    key: str                      # "<environment>/<experiment>", or "<experiment>"
    experiment: str
    environment: str | None
    cfg: ExperimentConfig

    @property
    def build_dir(self) -> str:
        return default_build_dir(self.cfg.memory.placement == "locks")


@dataclass
class Suite:
    name: str
    runs: list[PlannedRun]
    environments: list[Environment] = field(default_factory=list)
    latency_control: LatencyControlSpec | None = None
    overrides: list[str] = field(default_factory=list)
    source_text: str = ""
    cgroup: str | None = None     # cgroup.procs file the suite joins before building

    def environment(self, name: str | None) -> Environment | None:
        return next((env for env in self.environments if env.name == name), None)


def _table(value, where: str) -> dict:
    if not isinstance(value, dict):
        raise ConfigError(f"`{where}` must be a table")
    return value


def _check_keys(table: dict, allowed, prefix: str):
    for key in table:
        if key not in allowed:
            hint = get_close_matches(key, allowed, n=1)
            suggestion = f"; did you mean `{prefix}{hint[0]}`?" if hint else ""
            raise ConfigError(f"unknown suite setting `{prefix}{key}`{suggestion}")


def _check_name(name, where: str) -> str:
    if name is None:
        raise ConfigError(f"`{where}` is required")
    if not isinstance(name, str) or not _NAME.match(name):
        raise ConfigError(
            f"`{where}` must start with a letter or digit and use only letters, digits "
            f"and . _ + = -; got {name!r}"
        )
    return name


def _check_unique(names, what: str):
    seen = set()
    for name in names:
        if name in seen:
            hint = "; give one a `name`" if what == "experiment" else ""
            raise ConfigError(f"two {what}s are named {name!r}{hint}")
        seen.add(name)


def _merge(base: dict, overlay: dict) -> dict:
    """Deep-merge TOML tables; overlay wins."""
    merged = copy.deepcopy(base)
    for key, value in overlay.items():
        if isinstance(value, dict) and isinstance(merged.get(key), dict):
            merged[key] = _merge(merged[key], value)
        else:
            merged[key] = copy.deepcopy(value)
    return merged


def _latency_control(raw) -> LatencyControlSpec | None:
    if raw is None:
        return None
    raw = _table(raw, "latency_control")
    _check_keys(raw, _LATENCY_CONTROL_KEYS, "latency_control.")
    pci = raw.get("pci")
    if not isinstance(pci, str) or not _BDF.match(pci):
        raise ConfigError(
            '`latency_control.pci` must be the CXL device\'s PCI address on this host, '
            f'such as "0000:a0:00.0"; got {pci!r}'
        )
    register = raw.get("register", DEFAULT_LATENCY_REGISTER)
    if not isinstance(register, str) or not _REGISTER.match(register):
        raise ConfigError(f'`latency_control.register` must look like "0xe08.l"; got {register!r}')
    return LatencyControlSpec(pci, register)


def _environment(index: int, raw, latency_control) -> Environment:
    where = f"environments[{index}]"
    raw = _table(raw, where)
    _check_keys(raw, _ENVIRONMENT_KEYS, f"{where}.")
    name = _check_name(raw.get("name"), f"{where}.name")
    memory = raw.get("memory")
    if memory is not None:
        _table(memory, f"{where}.memory")
    latency = raw.get("latency")
    if latency is not None:
        match = _HEX_VALUE.match(latency) if isinstance(latency, str) else None
        if not match:
            raise ConfigError(
                f'`{where}.latency` must be a quoted hex register value such as "ffff"; got {latency!r}'
            )
        if latency_control is None:
            raise ConfigError(
                f"`{where}.latency` needs a [latency_control] table naming the CXL device's PCI address"
            )
        latency = match.group(1).lower()
    return Environment(name, copy.deepcopy(memory), latency)


def _vary_items(vary, where: str) -> list[tuple[str, list]]:
    """Flatten a vary table (dotted or nested keys) into (key, values) pairs."""
    items = []

    def walk(table, prefix):
        for key, value in table.items():
            dotted = f"{prefix}{key}"
            if isinstance(value, dict):
                walk(value, dotted + ".")
            elif isinstance(value, list) and value:
                items.append((dotted, value))
            else:
                raise ConfigError(f"`{where}.vary.{dotted}` must be a non-empty list of values")

    walk(_table(vary, f"{where}.vary"), "")
    return items


def _label(value) -> str:
    if isinstance(value, bool):
        text = "true" if value else "false"
    elif isinstance(value, list):
        text = "+".join(_label(item) for item in value)
    elif isinstance(value, float):
        text = f"{value:g}"
    else:
        text = str(value)
    return re.sub(r"[^A-Za-z0-9._+=-]", "_", text)


def _expand_experiment(index: int, raw, defaults: dict, base_dir: str, environment_names):
    """One [[experiments]] entry -> [(name, raw config data, sets memory, environments)].

    environments is the entry's `environments` filter, or None for all.
    """
    where = f"experiments[{index}]"
    entry = copy.deepcopy(_table(raw, where))
    file_ref = entry.pop("file", None)
    vary = entry.pop("vary", None)
    only_in = entry.pop("environments", None)
    if only_in is not None:
        if not environment_names:
            raise ConfigError(f"`{where}.environments` needs [[environments]] to choose from")
        if not isinstance(only_in, list) or not only_in or not all(isinstance(n, str) for n in only_in):
            raise ConfigError(f"`{where}.environments` must be a non-empty list of environment names")
        for env_name in only_in:
            if env_name not in environment_names:
                raise ConfigError(
                    f"`{where}.environments` names {env_name!r}, which is not an environment; "
                    f"choose from {', '.join(environment_names)}"
                )

    own, file_stem = {}, None
    if file_ref is not None:
        if not isinstance(file_ref, str):
            raise ConfigError(f"`{where}.file` must be a path to an experiment file")
        path = os.path.join(base_dir, file_ref)
        try:
            with open(path, "rb") as handle:
                own = tomllib.load(handle)
        except OSError as exc:
            raise ConfigError(f"`{where}.file`: cannot read {path}: {exc.strerror}") from None
        except tomllib.TOMLDecodeError as exc:
            raise ConfigError(f"`{where}.file`: {path} is not valid TOML: {exc}") from None
        file_stem = os.path.splitext(os.path.basename(file_ref))[0]
    own = _merge(own, entry)

    name = own.pop("name", None) or file_stem or own.get("bench") or defaults.get("bench") \
        or ExperimentConfig.bench
    _check_name(name, f"{where}.name")
    data = _merge(defaults, own)

    variations = _vary_items(vary, where) if vary is not None else []
    keys = [key for key, _ in variations]
    sets_memory = "memory" in own or any(k == "memory" or k.startswith("memory.") for k in keys)
    expanded = []
    for combination in itertools.product(*(values for _, values in variations)):
        run_data = copy.deepcopy(data)
        parts = [name]
        for key, value in zip(keys, combination):
            try:
                set_dotted(run_data, key, copy.deepcopy(value))
            except ConfigError as exc:
                raise ConfigError(f"`{where}.vary`: {exc}") from None
            parts.append(f"{key.rsplit('.', 1)[-1]}-{_label(value)}")
        expanded.append(("-".join(parts), run_data, sets_memory, only_in))
    return expanded


def suite_from_data(data: dict, overrides=(), *, base_dir: str = ".", default_name: str = "suite",
                    source_text: str = "") -> Suite:
    experiment_keys = {f.name for f in fields(ExperimentConfig)}
    for key in data:
        if key not in _SUITE_KEYS and key in experiment_keys:
            raise ConfigError(
                f"`{key}` is an experiment setting; put it under [defaults] or in an [[experiments]] entry"
            )
    _check_keys(data, _SUITE_KEYS, "")
    name = _check_name(data.get("name", default_name), "name")
    cgroup = data.get("cgroup")
    if cgroup is not None and (not isinstance(cgroup, str) or not os.path.isabs(cgroup)):
        raise ConfigError(f"`cgroup` must be the absolute path of a cgroup.procs file; got {cgroup!r}")
    defaults = copy.deepcopy(_table(data.get("defaults", {}), "defaults"))
    if "name" in defaults:
        raise ConfigError("`defaults.name` is not allowed; each experiment is named separately")
    latency_control = _latency_control(data.get("latency_control"))

    raw_environments = data.get("environments", [])
    if not isinstance(raw_environments, list):
        raise ConfigError("`environments` must be an array of tables ([[environments]])")
    environments = [_environment(i, raw, latency_control) for i, raw in enumerate(raw_environments)]
    _check_unique([env.name for env in environments], "environment")
    if "memory" in defaults and any(env.memory is not None for env in environments):
        raise ConfigError("`defaults.memory` conflicts with environments that set `memory`; set it in one place")

    raw_experiments = data.get("experiments")
    if not isinstance(raw_experiments, list) or not raw_experiments:
        raise ConfigError("a suite needs at least one [[experiments]] entry")
    environment_names = [env.name for env in environments]
    experiments = []
    for index, raw in enumerate(raw_experiments):
        experiments.extend(_expand_experiment(index, raw, defaults, base_dir, environment_names))
    _check_unique([exp_name for exp_name, _, _, _ in experiments], "experiment")

    runs = []
    for env in environments or [None]:
        for exp_name, exp_data, sets_memory, only_in in experiments:
            if only_in is not None and env.name not in only_in:
                continue
            key = f"{env.name}/{exp_name}" if env else exp_name
            run_data = copy.deepcopy(exp_data)
            if env is not None and env.memory is not None:
                if sets_memory:
                    raise ConfigError(
                        f"experiment {exp_name!r} sets `memory`, which environment {env.name!r} "
                        "also sets; set it in one place"
                    )
                run_data["memory"] = copy.deepcopy(env.memory)
            run_data["name"] = f"{env.name}-{exp_name}" if env else exp_name
            try:
                cfg = config_from_data(run_data, overrides)
            except ConfigError as exc:
                raise ConfigError(f"run {key}: {exc}") from None
            runs.append(PlannedRun(key, exp_name, env.name if env else None, cfg))
    return Suite(name, runs, environments, latency_control, list(overrides), source_text, cgroup)


def load_suite(path: str, overrides=()) -> Suite:
    """Load and expand a suite file; every run's config is validated up front."""
    try:
        with open(path, "rb") as handle:
            text = handle.read().decode("utf-8")
    except OSError as exc:
        raise ConfigError(f"cannot read suite {path}: {exc.strerror}") from None
    try:
        data = tomllib.loads(text)
        return suite_from_data(
            data, overrides,
            base_dir=os.path.dirname(os.path.abspath(path)),
            default_name=os.path.splitext(os.path.basename(path))[0],
            source_text=text,
        )
    except tomllib.TOMLDecodeError as exc:
        raise ConfigError(f"{path}: invalid TOML: {exc}") from None
    except ConfigError as exc:
        raise ConfigError(f"{path}: {exc}") from None


def run_folder(suite_dir: str, run: PlannedRun) -> str:
    return os.path.join(suite_dir, *run.key.split("/"))


def suite_from_record(suite_dir: str) -> Suite:
    """Rebuild a suite from its folder: suite.json plus each run's config.toml.

    Resuming never re-reads the original suite file, so editing or moving it
    (or the experiment files it names) cannot change a suite in progress.
    """
    record = read_json(os.path.join(suite_dir, SUITE_RECORD))
    if record is None:
        raise ConfigError(f"{suite_dir} has no readable {SUITE_RECORD}; it is not a suite folder")
    control = record.get("latency_control")
    environments = [
        Environment(env["name"], env.get("memory"), env.get("latency"))
        for env in record.get("environments", [])
    ]
    runs = []
    for entry in record.get("runs", []):
        run = PlannedRun(entry["key"], entry["experiment"], entry.get("environment"), None)
        run.cfg = load_config(os.path.join(run_folder(suite_dir, run), CONFIG_FILE))
        runs.append(run)
    return Suite(
        record["name"], runs, environments,
        LatencyControlSpec(control["pci"], control["register"]) if control else None,
        record.get("overrides", []),
        cgroup=record.get("cgroup"),
    )


# --------------------------------------------------------------------------
# Describing a plan (--list).
# --------------------------------------------------------------------------

@dataclass
class BuildPlan:
    dir: str
    hardware_cxl: bool
    mutex_names: list[str] = field(default_factory=list)
    tigon: bool = False


def plan_builds(runs) -> list[BuildPlan]:
    """One build per build folder, covering every lock and app its runs need."""
    plans: dict[str, BuildPlan] = {}
    for run in runs:
        plan = plans.setdefault(run.build_dir, BuildPlan(run.build_dir, run.cfg.memory.placement == "locks"))
        for lock in resolve_lock_names(run.cfg.locks):
            if lock not in plan.mutex_names:
                plan.mutex_names.append(lock)
        plan.tigon = plan.tigon or run.cfg.is_tigon
    return list(plans.values())


def executions(cfg: ExperimentConfig) -> int:
    points = len(range(cfg.sweep.start, cfg.sweep.stop + 1, cfg.sweep.step)) if cfg.sweep else 1
    return len(resolve_lock_names(cfg.locks)) * points * cfg.repetitions


def seconds_per_execution(cfg: ExperimentConfig) -> float:
    if cfg.is_tigon:
        return max(1, int(cfg.seconds)) + tigon_warmup_seconds(cfg.seconds, cfg.tigon.warmup_seconds)
    return cfg.seconds


def format_duration(seconds: float) -> str:
    seconds = int(round(seconds))
    hours, rest = divmod(seconds, 3600)
    minutes, seconds = divmod(rest, 60)
    return f"{hours}h {minutes:02d}m {seconds:02d}s" if hours else f"{minutes}m {seconds:02d}s"


def _placement_text(cfg: ExperimentConfig) -> str:
    if cfg.memory.placement == "none":
        return "none"
    return f"{cfg.memory.placement} node {cfg.memory.node}"


def _environment_text(suite: Suite, env: Environment) -> str:
    parts = []
    if env.memory is not None:
        parts.append(", ".join(f"memory.{key} = {value}" for key, value in env.memory.items()))
    control = suite.latency_control
    if env.latency is not None:
        parts.append(f"CXL latency {control.register}={env.latency} on {control.pci}")
    elif control is not None:
        parts.append("CXL latency at the host's setting")
    return "; ".join(parts) or "as configured"


def describe_suite(suite: Suite, statuses: dict[str, str] | None = None) -> str:
    lines = [f"Suite {suite.name}: {len(suite.runs)} runs"]
    experiments = len({run.experiment for run in suite.runs})
    if suite.environments and len(suite.runs) == len(suite.environments) * experiments:
        lines[0] += f" ({len(suite.environments)} environments x {experiments} experiments)"
    if suite.cgroup:
        lines.append(f"Runs in the cgroup of {suite.cgroup}")
    if suite.environments:
        lines += ["", "Environments:"]
        width = max(len(env.name) for env in suite.environments)
        lines += [f"  {env.name:<{width}}  {_environment_text(suite, env)}" for env in suite.environments]

    width = max(len(run.key) for run in suite.runs)
    header = f"  {'run':<{width}}  {'bench':<10}  {'placement':<15}  {'locks':>5}  {'points':>6}  {'reps':>4}  {'execs':>6}  {'time':>12}"
    if statuses is not None:
        header += "  status"
    lines += ["", "Runs:", header]
    total_executions, total_seconds = 0, 0.0
    for run in suite.runs:
        cfg = run.cfg
        count = executions(cfg)
        seconds = count * seconds_per_execution(cfg)
        total_executions += count
        total_seconds += seconds
        points = len(range(cfg.sweep.start, cfg.sweep.stop + 1, cfg.sweep.step)) if cfg.sweep else 1
        line = (
            f"  {run.key:<{width}}  {cfg.bench:<10}  {_placement_text(cfg):<15}  "
            f"{len(resolve_lock_names(cfg.locks)):>5}  {points:>6}  {cfg.repetitions:>4}  "
            f"{count:>6}  {format_duration(seconds):>12}"
        )
        if statuses is not None:
            line += f"  {statuses.get(run.key, 'pending')}"
        lines.append(line)

    lines += ["", "Builds:"]
    for plan in plan_builds(suite.runs):
        args = " ".join(required_cpp_args(plan.hardware_cxl, plan.mutex_names))
        lines.append(f"  {plan.dir + '/':<20}  {args}{'  + Tigon' if plan.tigon else ''}")
    if suite.overrides:
        lines += ["", "Overrides: " + " ".join(f"-o {override}" for override in suite.overrides)]
    lines += [
        "",
        f"Total: {total_executions} benchmark executions, about {format_duration(total_seconds)} of measured "
        "time (excludes builds, startup and plotting).",
    ]
    return "\n".join(lines)


# --------------------------------------------------------------------------
# Host state: the CXL latency register.
# --------------------------------------------------------------------------

def _run_setpci(command: list[str]) -> str:
    if os.geteuid() != 0:
        command = ["sudo", *command]
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"`{' '.join(command)}` failed: {result.stderr.strip() or result.returncode}")
    return result.stdout


def write_cgroup(path: str, pid: str):
    """Move a process into a cgroup by writing its PID to cgroup.procs (via sudo)."""
    if os.geteuid() == 0:
        with open(path, "w", encoding="utf-8") as handle:
            handle.write(pid + "\n")
        return
    result = subprocess.run(["sudo", "tee", path], input=pid + "\n", stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE, text=True, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"could not join the cgroup of {path}: {result.stderr.strip()}")


class LatencyControl:
    """Reads, sets and restores the CXL latency register with setpci.

    The original value is saved before the first change and written back
    when the suite ends, however it ends (including Ctrl-C and SIGTERM).
    """

    def __init__(self, spec: LatencyControlSpec, run_command=None):
        self.spec = spec
        self._run = run_command or _run_setpci
        self.original: str | None = None
        self.current: str | None = None

    @staticmethod
    def check_available():
        if not sys.platform.startswith("linux"):
            raise ConfigError("CXL latency control requires Linux")
        if shutil.which("setpci") is None:
            raise ConfigError("CXL latency control requires setpci (pciutils) on PATH")

    def _read(self) -> str:
        value = self._run(["setpci", "-s", self.spec.pci, self.spec.register]).strip()
        match = _HEX_VALUE.match(value)
        if not match:
            raise RuntimeError(f"could not read the CXL latency register: setpci printed {value!r}")
        return match.group(1).lower()

    def save_original(self) -> str:
        self.original = self.current = self._read()
        return self.original

    def apply(self, value: str):
        # Compare numerically: setpci prints .l registers zero-padded to 8 digits.
        if self.current is not None and int(value, 16) == int(self.current, 16):
            return
        self._run(["setpci", "-s", self.spec.pci, f"{self.spec.register}={value}"])
        self.current = None  # unknown until the readback succeeds
        actual = self._read()
        if int(actual, 16) != int(value, 16):
            raise RuntimeError(f"CXL latency readback mismatch: wrote {value}, read {actual}")
        self.current = value

    def restore(self):
        if self.original is not None:
            self.apply(self.original)

    def restore_command(self) -> str:
        return f"sudo setpci -s {self.spec.pci} {self.spec.register}={self.original}"


# --------------------------------------------------------------------------
# Running a suite.
# --------------------------------------------------------------------------

class SuiteTerminated(KeyboardInterrupt):
    """SIGTERM, handled like Ctrl-C so host state is restored."""


def _raise_terminated(signum, frame):
    raise SuiteTerminated()


def run_child(command: list[str], env: dict) -> int:
    """Run one experiment process; on interruption, let it record itself first."""
    process = subprocess.Popen(command, env=env)
    try:
        return process.wait()
    except KeyboardInterrupt as exc:
        if isinstance(exc, SuiteTerminated):
            process.send_signal(signal.SIGINT)  # Ctrl-C already reached the child
        try:
            process.wait(timeout=_CHILD_EXIT_GRACE_SECONDS)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        raise


@dataclass
class SuiteOptions:
    only: list[str] = field(default_factory=list)
    no_build: bool = False
    stop_on_failure: bool = False


class SuiteRunner:
    def __init__(self, suite: Suite, suite_dir: str, options: SuiteOptions | None = None, *,
                 child=run_child, setpci=None, cgroup_writer=None, out=None):
        self.suite = suite
        self._cgroup_writer = cgroup_writer or write_cgroup
        self.suite_dir = suite_dir
        self.options = options or SuiteOptions()
        self._child = child
        self._setpci = setpci
        self._out = out or sys.stdout
        self.record_path = os.path.join(suite_dir, SUITE_RECORD)
        self.record: dict = read_json(self.record_path) or {}

    def _print(self, message: str = ""):
        print(message, file=self._out, flush=True)

    def _write(self):
        write_json_atomic(self.record_path, self.record)

    def _entry(self, run: PlannedRun) -> dict:
        return next(entry for entry in self.record["runs"] if entry["key"] == run.key)

    def status(self, run: PlannedRun) -> str:
        record = read_record(run_folder(self.suite_dir, run))
        return record.get("status", "unknown") if record else "pending"

    # -- setup ---------------------------------------------------------------

    def create(self, source: str | None = None):
        """Make a new suite folder with a prepared run folder per run."""
        if os.path.exists(self.record_path):
            raise ConfigError(f"{self.suite_dir} already holds a suite; resume it with --resume")
        os.makedirs(self.suite_dir, exist_ok=True)
        with open(os.path.join(self.suite_dir, SUITE_FILE), "w", encoding="utf-8") as handle:
            handle.write(self.suite.source_text)
        for run in self.suite.runs:
            self._prepare_run_folder(run)
        control = self.suite.latency_control
        self.record = {
            "name": self.suite.name,
            "status": "planned",
            "created_utc": utc_now(),
            "source": source,
            "overrides": self.suite.overrides,
            "cgroup": self.suite.cgroup,
            "latency_control": {"pci": control.pci, "register": control.register} if control else None,
            "environments": [
                {"name": env.name, "memory": env.memory, "latency": env.latency}
                for env in self.suite.environments
            ],
            "runs": [
                {"key": run.key, "experiment": run.experiment, "environment": run.environment,
                 "status": "pending"}
                for run in self.suite.runs
            ],
            "sessions": [],
            "git": git_info(),
            "host": host_info(),
        }
        self._write()

    def _prepare_run_folder(self, run: PlannedRun):
        folder = run_folder(self.suite_dir, run)
        os.makedirs(folder, exist_ok=True)
        with open(os.path.join(folder, CONFIG_FILE), "w", encoding="utf-8") as handle:
            handle.write(to_toml(run.cfg))

    def selected_runs(self) -> list[PlannedRun]:
        patterns = self.options.only
        if not patterns:
            return list(self.suite.runs)

        def matches(run, pattern):
            return any(
                fnmatch.fnmatchcase(name, pattern)
                for name in (run.key, run.experiment, run.environment or "")
            )

        chosen = [run for run in self.suite.runs if any(matches(run, p) for p in patterns)]
        if not chosen:
            examples = ", ".join(run.key for run in self.suite.runs[:3])
            raise ConfigError(f"no runs match --only {' '.join(patterns)}; runs are named like {examples}")
        return chosen

    def _check_host(self, pending: list[PlannedRun]):
        if self.suite.cgroup and not os.path.exists(self.suite.cgroup):
            raise ConfigError(
                f"cgroup file {self.suite.cgroup} does not exist; create the cgroup "
                "or remove `cgroup` from the suite"
            )
        checked = set()
        for run in pending:
            placement = (run.cfg.memory.placement, run.cfg.memory.node)
            if placement not in checked:
                check_host(run.cfg)
                checked.add(placement)
        if self._needs_latency(pending) and self._setpci is None:
            LatencyControl.check_available()

    def _needs_latency(self, runs) -> bool:
        return any(
            (env := self.suite.environment(run.environment)) is not None and env.latency is not None
            for run in runs
        )

    # -- the run loop ----------------------------------------------------------

    def run(self) -> int:
        """Run every selected run that is not complete. Returns an exit code."""
        selected = self.selected_runs()
        pending = [run for run in selected if self.status(run) != "complete"]
        done = len(selected) - len(pending)
        self._print(f"Suite {self.suite.name}: {len(pending)} of {len(selected)} selected runs to do"
                    + (f" ({done} already complete)" if done else "") + f"  [{self.suite_dir}]")
        if not pending:
            return 0
        self._check_host(pending)

        session = {"command": command_line(), "started_utc": utc_now(), "only": self.options.only}
        self.record.setdefault("sessions", []).append(session)
        self.record["status"] = "running"
        self._write()

        latency = None
        status = "failed"
        previous_handler = signal.signal(signal.SIGTERM, _raise_terminated)
        try:
            if self.suite.cgroup:
                self._join_cgroup(self.suite.cgroup)
                session["cgroup_joined"] = self.suite.cgroup
                self._print(f"==> Joined the cgroup of {self.suite.cgroup}; builds and runs inherit it")
            self._build(pending, session)
            if self._needs_latency(pending):
                latency = LatencyControl(self.suite.latency_control, self._setpci)
                session["latency_original"] = latency.save_original()
                self._write()
                self._print(f"==> Saved CXL latency {latency.spec.register}={latency.original}; "
                            f"if this process is killed, restore it with: {latency.restore_command()}")
            self._run_pending(pending, latency)
            status = "complete" if all(self.status(run) == "complete" for run in self.suite.runs) \
                else "incomplete"
        except KeyboardInterrupt:
            status = "interrupted"
            raise
        except BaseException as exc:
            session["error"] = f"{type(exc).__name__}: {exc}"
            raise
        finally:
            signal.signal(signal.SIGTERM, previous_handler)
            if latency is not None:
                self._restore_latency(latency, session)
            session["finished_utc"] = utc_now()
            session["status"] = status
            self.record["status"] = status
            self._write()
            self._print_summary(selected)
        failed = [run for run in selected if self.status(run) != "complete"]
        return 1 if failed else 0

    def _join_cgroup(self, path: str):
        self._cgroup_writer(path, str(os.getpid()))

    def _build(self, pending: list[PlannedRun], session: dict):
        session["builds"] = []
        for plan in plan_builds(pending):
            if self.options.no_build:
                self._print(f"==> Reusing {plan.dir}/ (--no-build); each run checks that it fits")
                session["builds"].append({"dir": plan.dir, "skipped": True})
                continue
            self._print(f"==> Building {plan.dir}/")
            build(plan.dir, hardware_cxl=plan.hardware_cxl, mutex_names=plan.mutex_names, tigon=plan.tigon)
            session["builds"].append({"dir": plan.dir, "cpp_args": configured_cpp_args(plan.dir), "skipped": False})
            self._write()

    def _run_pending(self, pending: list[PlannedRun], latency: LatencyControl | None):
        total = len(pending)
        index = 0
        for env_name, group in itertools.groupby(pending, key=lambda run: run.environment):
            env = self.suite.environment(env_name)
            if latency is not None and env is not None:
                if env.latency is not None:
                    latency.apply(env.latency)
                    self._print(f"==> Environment {env.name}: CXL latency set to {env.latency}")
                else:
                    latency.restore()
                    self._print(f"==> Environment {env.name}: CXL latency at the host's setting ({latency.original})")
            for run in group:
                index += 1
                ok = self._run_one(run, env, latency, index, total)
                if not ok and self.options.stop_on_failure:
                    self._print("==> Stopping at the first failure (--stop-on-failure)")
                    return

    def _run_one(self, run: PlannedRun, env: Environment | None, latency, index: int, total: int) -> bool:
        folder = run_folder(self.suite_dir, run)
        entry = self._entry(run)
        if read_record(folder) is not None:  # an earlier attempt that did not complete
            attempt = self._move_aside(folder)
            entry.setdefault("attempts", []).append(os.path.relpath(attempt, self.suite_dir))
            self._prepare_run_folder(run)
            self._print(f"==> Earlier attempt of {run.key} moved to {attempt}")
        entry["status"] = "running"
        self._write()

        self._print(f"\n==> [{index}/{total}] {run.key}")
        command = [
            sys.executable, "-m", "scripts", "run", os.path.join(folder, CONFIG_FILE),
            "--dir", folder, "--build-dir", run.build_dir, "--no-build",
        ]
        context = {"name": self.suite.name, "dir": self.suite_dir, "run": run.key,
                   "cgroup": self.suite.cgroup,
                   "environment": self._environment_context(env, latency)}
        child_env = {**os.environ, SUITE_CONTEXT_ENV: json.dumps(context)}
        code = None
        try:
            code = self._child(command, child_env)
        finally:
            self._record_result(entry, folder, code)
        return entry["status"] == "complete"

    def _record_result(self, entry: dict, folder: str, code: int | None):
        result = read_record(folder)
        entry["exit_code"] = code
        if result is None:
            entry["status"] = "failed"
            entry["error"] = "the run exited before recording itself; see the output above"
        else:
            status = result.get("status")
            if status == "running":  # killed before it could record how it ended
                status = "failed" if code is not None else "interrupted"
            entry["status"] = status
            entry["data_points"] = result.get("data_points")
            if result.get("error"):
                entry["error"] = result["error"]
            else:
                entry.pop("error", None)
        self._write()

    def _environment_context(self, env: Environment | None, latency) -> dict | None:
        if env is None:
            return None
        context = {"name": env.name, "memory": env.memory}
        control = self.suite.latency_control
        if control is not None:
            if env.latency is not None:
                context["cxl_latency"] = {"pci": control.pci, "register": control.register, "value": env.latency}
            else:
                original = latency.original if latency is not None else None
                context["cxl_latency"] = {"pci": control.pci, "register": control.register,
                                          "value": original, "host_setting": True}
        return context

    @staticmethod
    def _move_aside(folder: str) -> str:
        attempt = 1
        while os.path.exists(f"{folder}.attempt-{attempt}"):
            attempt += 1
        target = f"{folder}.attempt-{attempt}"
        os.rename(folder, target)
        return target

    def _restore_latency(self, latency: LatencyControl, session: dict):
        try:
            latency.restore()
            self._print(f"==> Restored CXL latency {latency.spec.register}={latency.original}")
        except Exception as exc:  # never mask the suite's own outcome
            session["latency_restore_error"] = str(exc)
            self._print(f"ERROR: could not restore the CXL latency ({exc}); run: {latency.restore_command()}")

    def _print_summary(self, selected: list[PlannedRun]):
        self._print(f"\nSuite {self.suite.name} [{self.suite_dir}]")
        width = max(len(run.key) for run in selected)
        for run in selected:
            entry = self._entry(run)
            status = entry.get("status", "pending")
            points = entry.get("data_points") or {}
            detail = f"{points['written']}/{points['expected']} points" if points else ""
            if status != "complete" and entry.get("error"):
                detail = (detail + "  " if detail else "") + entry["error"].splitlines()[0][:100]
            self._print(f"  {run.key:<{width}}  {status:<11}  {detail}")


def new_suite_dir(name: str, root: str = SUITES_ROOT) -> str:
    return os.path.join(root, f"{timestamp()}-{name}")
