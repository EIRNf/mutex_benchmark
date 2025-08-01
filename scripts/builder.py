from .constants import *
from .logger import logger

import subprocess
import os

def setup():
    # Make sure the script is being run from the right location (in mutex_benchmark directory)
    absolute_path = os.path.abspath(__file__)
    parent_directory = os.path.dirname(absolute_path)
    os.chdir(parent_directory + "/..")
    
#TODO: Make this more robust. Add option to use existing build directory.
def build():
    subprocess.run(f"mkdir build data {Constants.Defaults.DATA_FOLDER} {Constants.Defaults.LOGS_FOLDER}".split()) 

    # Compile
    result = subprocess.run("meson setup build".split())#, stdout=subprocess.DEVNULL)
    assert result.returncode == 0, "Meson build failed."
    configure_command = "meson configure build --optimization 3".split()

    cpp_args = []
    if Constants.cxl:
        cpp_args.append("'-Dcxl'")
    cpp_args.append("'-mwaitpkg'")
    cpp_args.append("'-std=c++20'")
    for mutex_name in Constants.Defaults.CONDITIONAL_COMPILATION_MUTEXES:
        if mutex_name in Constants.mutex_names:
            cpp_args.append(f"'-Dinc_{mutex_name}'")
    configure_command.append(f'-Dcpp_args=[{", ".join(cpp_args)}]')
    print(configure_command)

    result = subprocess.run(configure_command)
    assert result.returncode == 0, "Configuration failed." #, stdout=subprocess.DEVNULL)
    result = subprocess.run("meson compile -C build".split())#, stdout=subprocess.DEVNULL)
    assert result.returncode == 0, "Compilation failed."