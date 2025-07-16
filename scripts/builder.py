from .constants import *

import subprocess
import os

def build():
    subprocess.run(f"mkdir build data {Constants.Defaults.DATA_FOLDER} {Constants.Defaults.LOGS_FOLDER} -p".split())

    # Compile
    result = subprocess.run("meson setup build".split())#, stdout=subprocess.DEVNULL)
    assert result.returncode == 0, "Meson build failed."
    subprocess.run('meson configure build --buildtype debugoptimized'.split())#, stdout=subprocess.DEVNULL)
    result = subprocess.run("meson compile -C build".split())#, stdout=subprocess.DEVNULL)
    assert result.returncode == 0, "Compilation failed."