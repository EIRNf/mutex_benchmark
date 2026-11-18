"""Self-describing run folders.

Every run writes two files beside its data:

  config.toml  the fully resolved ExperimentConfig. Re-running it (with
               ``python -m scripts run config.toml``) repeats the experiment
               in a new folder; ``python -m scripts plot <dir>`` re-plots this
               one without any flags.
  run.json     what actually happened: status (running / complete / failed /
               interrupted), timing, the command, git and host details, the
               resolved lock list, the build's configured compiler flags, and
               how many of the expected data points were written.
"""

from __future__ import annotations

import json
import os
import platform
import shlex
import shutil
import socket
import subprocess
import sys
from datetime import datetime, timezone

from .config import CONFIG_FILE, RECORD_FILE, ExperimentConfig, to_toml
from .constants import Constants
from .runner import get_data_file_name, placement_tag

_MAX_LISTED_MISSING = 50

# Set by the suite runner for each run it starts: JSON naming the suite, the
# run's key and its environment (including any CXL latency setting), which
# run.json records under "suite".
SUITE_CONTEXT_ENV = "MUTEX_BENCH_SUITE_CONTEXT"


def utc_now() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _capture(command, timeout=10) -> str | None:
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    except (OSError, subprocess.SubprocessError):
        return None
    return result.stdout.strip() if result.returncode == 0 else None


def command_line() -> str:
    """The invoking command, with `python -m` module runs shown as such."""
    argv = list(sys.argv)
    script = argv[0].replace(os.sep, "/") if argv else ""
    for suffix, module in (("scripts/__main__.py", "scripts"), ("scripts/main.py", "scripts.main")):
        if script.endswith(suffix):
            argv = ["-m", module, *argv[1:]]
            break
    return " ".join(shlex.quote(arg) for arg in [sys.executable, *argv])


def git_info(path: str = ".") -> dict | None:
    commit = _capture(["git", "-C", path, "rev-parse", "HEAD"])
    if commit is None:
        return None
    status = _capture(["git", "-C", path, "status", "--porcelain", "--untracked-files=no"])
    return {
        "commit": commit,
        "branch": _capture(["git", "-C", path, "rev-parse", "--abbrev-ref", "HEAD"]),
        "dirty": bool(status),
    }


def host_info() -> dict:
    info = {
        "hostname": socket.gethostname(),
        "platform": platform.platform(),
        "python": platform.python_version(),
    }
    if shutil.which("numactl"):
        info["numactl_hardware"] = _capture(["numactl", "--hardware"])
    return info


def expected_data_files() -> list[str]:
    """Every CSV path the configured experiment should produce."""
    paths = []
    for rep in range(Constants.n_program_iterations):
        if Constants.iter:
            for value in range(*Constants.iter_range):
                for mutex_name in Constants.mutex_names:
                    paths.append(get_data_file_name(
                        mutex_name, rep, **{Constants.iter_variable_name: value}
                    ))
        else:
            for mutex_name in Constants.mutex_names:
                paths.append(get_data_file_name(mutex_name, rep))
    return paths


def data_point_summary() -> dict:
    expected = expected_data_files()
    missing = [
        os.path.basename(path) for path in expected
        if not (os.path.isfile(path) and os.path.getsize(path) > 0)
    ]
    summary = {"expected": len(expected), "written": len(expected) - len(missing)}
    if missing:
        summary["missing"] = missing[:_MAX_LISTED_MISSING]
        if len(missing) > _MAX_LISTED_MISSING:
            summary["missing_not_listed"] = len(missing) - _MAX_LISTED_MISSING
    return summary


class RunRecord:
    """Writes config.toml once and keeps run.json current for one run folder."""

    def __init__(self, cfg: ExperimentConfig, run_dir: str):
        self.run_dir = run_dir
        self.record_path = os.path.join(run_dir, RECORD_FILE)
        self.data: dict = {}
        self._cfg = cfg

    def start(self):
        os.makedirs(self.run_dir, exist_ok=True)
        with open(os.path.join(self.run_dir, CONFIG_FILE), "w", encoding="utf-8") as handle:
            handle.write(to_toml(self._cfg))
        self.data = {
            "name": self._cfg.name,
            "status": "running",
            "started_utc": utc_now(),
            "finished_utc": None,
            "command": command_line(),
            "data_folder": Constants.data_folder,
            "plots_folder": Constants.figs_folder,
            "logs_folder": Constants.logs_folder,
            "locks": list(Constants.mutex_names),
            "placement": {
                "mode": Constants.placement,
                "node": Constants.numa_node,
                "tag": placement_tag() or None,
            },
            "git": git_info(),
            "host": host_info(),
        }
        if self._cfg.is_tigon:
            self.data["tigon_submodule"] = git_info("apps/tigon_db_bench/tigon")
            # Tigon's allocator backend; null means unset, which Tigon treats as "mmap".
            self.data["cxlalloc_backend"] = os.environ.get("CXLALLOC_BACKEND") or None
        suite = _suite_context()
        if suite is not None:
            self.data["suite"] = suite
        self._write()

    def update(self, **fields):
        self.data.update(fields)
        self._write()

    def finish(self, status: str, error: str | None = None):
        self.data["status"] = status
        self.data["finished_utc"] = utc_now()
        if error is not None:
            self.data["error"] = error
        self.data["data_points"] = data_point_summary()
        self._write()

    def _write(self):
        write_json_atomic(self.record_path, self.data)


def write_json_atomic(path: str, data) -> None:
    """Write JSON so readers never see a half-written file."""
    tmp_path = path + ".tmp"
    with open(tmp_path, "w", encoding="utf-8") as handle:
        json.dump(data, handle, indent=2)
        handle.write("\n")
    os.replace(tmp_path, path)


def read_json(path: str) -> dict | None:
    try:
        with open(path, encoding="utf-8") as handle:
            data = json.load(handle)
    except (OSError, json.JSONDecodeError):
        return None
    return data if isinstance(data, dict) else None


def _suite_context() -> dict | None:
    raw = os.environ.get(SUITE_CONTEXT_ENV)
    if not raw:
        return None
    try:
        context = json.loads(raw)
    except ValueError:
        return None
    return context if isinstance(context, dict) else None


def read_record(run_dir: str) -> dict | None:
    return read_json(os.path.join(run_dir, RECORD_FILE))
