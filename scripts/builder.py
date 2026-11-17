from .constants import *
from .logger import logger

import json
import subprocess
import os

BUILD_DIR = "build"

def setup():
    # Make sure the script is being run from the right location (in mutex_benchmark directory)
    absolute_path = os.path.abspath(__file__)
    parent_directory = os.path.dirname(absolute_path)
    os.chdir(parent_directory + "/..")


def _run_checked(command, error_message):
    logger.debug(f"Running command: {command}")
    result = subprocess.run(command)
    if result.returncode != 0:
        raise RuntimeError(error_message)


def prepare_output_dirs():
    """Create the folders the configured run reads from or writes to."""
    for folder in (Constants.data_folder, Constants.figs_folder, Constants.logs_folder):
        os.makedirs(folder, exist_ok=True)


def configured_cpp_args(build_dir=BUILD_DIR):
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


def verify_build_matches(build_dir=BUILD_DIR):
    """Fail if the build directory was configured for a different placement mode.

    Matters when MUTEX_BENCH_SKIP_BUILD=1 reuses an existing build: a malloc
    build cannot honor placement = "locks", and a hardware_cxl build would
    place lock memory when it should not.
    """
    cpp_args = configured_cpp_args(build_dir)
    if cpp_args is None:
        raise RuntimeError(f"{build_dir}/ is not a configured Meson build; build without MUTEX_BENCH_SKIP_BUILD")
    built_for_locks = "-Dhardware_cxl" in cpp_args
    if built_for_locks != bool(Constants.hardware_cxl):
        built = "with" if built_for_locks else "without"
        raise RuntimeError(
            f"{build_dir}/ is configured {built} -Dhardware_cxl, which does not match "
            f"placement = {Constants.placement!r}; rebuild (unset MUTEX_BENCH_SKIP_BUILD)"
        )
    return cpp_args


def build():
    os.makedirs(BUILD_DIR, exist_ok=True)

    if os.environ.get("MUTEX_BENCH_SKIP_BUILD") == "1":
        logger.info("Skipping build because MUTEX_BENCH_SKIP_BUILD=1")
        return

    meson_setup_cmd = ["meson", "setup", "build"]
    if os.path.exists("build/meson-info/meson-info.json"):
        meson_setup_cmd.append("--reconfigure")
    _run_checked(meson_setup_cmd, "Meson setup failed.")
    configure_command = "meson configure build --optimization 3".split()

    cpp_args = []
    if Constants.hardware_cxl:
        cpp_args.append("'-Dhardware_cxl'")
        cpp_args.append("'-lnuma'")
    # cpp_args.append("'-mwaitpkg'")
    cpp_args.append("'-std=c++20'")
    for mutex_name in Constants.Defaults.CONDITIONAL_COMPILATION_MUTEXES:
        if mutex_name in Constants.mutex_names:
            cpp_args.append(f"'-Dinc_{mutex_name}'")
    configure_command.append(f'-Dcpp_args=[{",".join(cpp_args)}]')
    print(configure_command)

    _run_checked(configure_command, "Meson configuration failed.")
    _run_checked("meson compile -C build".split(), "Meson compilation failed.")

    if Constants.bench.startswith("tigon"):
        logger.info("Building Tigon benchmark suite...")
        tigon_build_script = "./apps/tigon_db_bench/build.sh"
        if os.path.exists(tigon_build_script):
            _run_checked([tigon_build_script, "./build/apps/tigon_db_bench"], "Tigon build failed.")
