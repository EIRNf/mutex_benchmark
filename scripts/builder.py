from .constants import *
from .logger import logger

import json
import os
import shutil
import subprocess
import sys

DEFAULT_BUILD_DIR = "build"
HARDWARE_CXL_BUILD_DIR = "build-hardware_cxl"
TIGON_BUILD_SCRIPT = "./apps/tigon_db_bench/build.sh"


def default_build_dir(hardware_cxl: bool) -> str:
    """Builds with and without -Dhardware_cxl live in separate folders, so
    switching placement between runs never forces a full rebuild."""
    return HARDWARE_CXL_BUILD_DIR if hardware_cxl else DEFAULT_BUILD_DIR


def required_cpp_args(hardware_cxl: bool, mutex_names) -> list[str]:
    """The compiler arguments a build needs for this placement and lock list."""
    args = []
    if hardware_cxl:
        args += ["-Dhardware_cxl", "-lnuma"]
    args.append("-std=c++20")
    args += [
        f"-Dinc_{name}" for name in Constants.Defaults.CONDITIONAL_COMPILATION_MUTEXES
        if name in mutex_names
    ]
    return args


def setup():
    # Make sure the script is being run from the right location (in mutex_benchmark directory)
    absolute_path = os.path.abspath(__file__)
    parent_directory = os.path.dirname(absolute_path)
    os.chdir(parent_directory + "/..")


def _build_env() -> dict:
    """The environment for build commands.

    Puts the running interpreter's bin directory on PATH, so meson and ninja
    installed in this Python's virtualenv (pip install -r requirements.txt)
    are found even when the virtualenv is not activated, e.g. when running
    `venv/bin/python -m scripts ...` directly or from an IDE.
    """
    env = os.environ.copy()
    interpreter_bin = os.path.dirname(sys.executable)
    path = env.get("PATH", "")
    if interpreter_bin and interpreter_bin not in path.split(os.pathsep):
        env["PATH"] = os.pathsep.join(p for p in (interpreter_bin, path) if p)
    return env


def _run_checked(command, error_message):
    logger.debug(f"Running command: {command}")
    env = _build_env()
    if shutil.which(command[0], path=env["PATH"]) is None:
        raise RuntimeError(
            f"{error_message} `{command[0]}` was not found on PATH or next to {sys.executable}. "
            "Install the build tools (python3 -m pip install -r requirements.txt, "
            "or brew/apt install meson ninja) or activate the virtualenv that has them."
        )
    result = subprocess.run(command, env=env)
    if result.returncode != 0:
        raise RuntimeError(error_message)


def prepare_output_dirs():
    """Create the folders the configured run reads from or writes to."""
    for folder in (Constants.data_folder, Constants.figs_folder, Constants.logs_folder):
        os.makedirs(folder, exist_ok=True)


def configured_cpp_args(build_dir):
    """The cpp_args a Meson build directory was configured with, or None."""
    try:
        with open(os.path.join(build_dir, "meson-info", "intro-buildoptions.json"), encoding="utf-8") as handle:
            options = json.load(handle)
    except (OSError, json.JSONDecodeError):
        return None
    for option in options:
        if option.get("name") == "cpp_args":
            return list(option.get("value") or [])
    return []


def verify_build_matches(build_dir, *, hardware_cxl: bool, mutex_names, placement: str = ""):
    """Fail if a build directory cannot run this configuration.

    Matters whenever an existing build is reused (--no-build,
    MUTEX_BENCH_SKIP_BUILD=1, or a suite's shared build): a malloc build
    cannot honor placement = "locks", a hardware_cxl build would place lock
    memory when it should not, and conditionally compiled locks (nsync,
    boost, umwait, futex) are missing unless the build enabled them.
    """
    cpp_args = configured_cpp_args(build_dir)
    if cpp_args is None:
        raise RuntimeError(f"{build_dir}/ is not a configured Meson build; run without --no-build to build it")
    built_for_locks = "-Dhardware_cxl" in cpp_args
    if built_for_locks != bool(hardware_cxl):
        built = "with" if built_for_locks else "without"
        raise RuntimeError(
            f"{build_dir}/ is configured {built} -Dhardware_cxl, which does not match "
            f"placement = {placement!r}; rebuild it (run without --no-build)"
        )
    missing = [
        arg for arg in required_cpp_args(hardware_cxl, mutex_names)
        if arg.startswith("-Dinc_") and arg not in cpp_args
    ]
    if missing:
        raise RuntimeError(
            f"{build_dir}/ was built without {', '.join(missing)}, which the selected locks "
            "need; rebuild it (run without --no-build)"
        )
    return cpp_args


def build(build_dir, *, hardware_cxl: bool, mutex_names, tigon: bool):
    """Configure and compile one build directory (and Tigon into it, if asked)."""
    os.makedirs(build_dir, exist_ok=True)
    setup_command = ["meson", "setup", build_dir]
    if os.path.exists(os.path.join(build_dir, "meson-info", "meson-info.json")):
        setup_command.append("--reconfigure")
    _run_checked(setup_command, "Meson setup failed.")

    cpp_args = ",".join(f"'{arg}'" for arg in required_cpp_args(hardware_cxl, mutex_names))
    configure_command = ["meson", "configure", build_dir, "--optimization", "3", f"-Dcpp_args=[{cpp_args}]"]
    logger.info("Configuring %s: %s", build_dir, " ".join(configure_command))
    _run_checked(configure_command, "Meson configuration failed.")
    _run_checked(["meson", "compile", "-C", build_dir], "Meson compilation failed.")

    if tigon:
        logger.info("Building Tigon benchmark suite into %s...", build_dir)
        if os.path.exists(TIGON_BUILD_SCRIPT):
            _run_checked(
                [TIGON_BUILD_SCRIPT, os.path.join(build_dir, "apps", "tigon_db_bench")],
                "Tigon build failed.",
            )
