from .constants import *
from .logger import logger

import subprocess
import os

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


def build():
    os.makedirs("build", exist_ok=True)
    os.makedirs("data", exist_ok=True)
    os.makedirs(Constants.Defaults.DATA_FOLDER, exist_ok=True)
    os.makedirs(Constants.Defaults.LOGS_FOLDER, exist_ok=True)

    meson_setup_cmd = ["meson", "setup", "build"]
    if os.path.exists("build/meson-info/meson-info.json"):
        meson_setup_cmd.append("--reconfigure")
    _run_checked(meson_setup_cmd, "Meson setup failed.")
    configure_command = "meson configure build --optimization 3".split()

    cpp_args = []
    if Constants.hardware_cxl:
        cpp_args.append("'-Dhardware_cxl'")
        cpp_args.append("'-lnuma'")
    elif Constants.software_cxl:
        cpp_args.append("'-Dsoftware_cxl'")
    # cpp_args.append("'-mwaitpkg'")
    cpp_args.append("'-std=c++20'")
    for mutex_name in Constants.Defaults.CONDITIONAL_COMPILATION_MUTEXES:
        if mutex_name in Constants.mutex_names:
            cpp_args.append(f"'-Dinc_{mutex_name}'")
    configure_command.append(f'-Dcpp_args=[{",".join(cpp_args)}]')
    print(configure_command)

    _run_checked(configure_command, "Meson configuration failed.")
    _run_checked("meson compile -C build".split(), "Meson compilation failed.")
