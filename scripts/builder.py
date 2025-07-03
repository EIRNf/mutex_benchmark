from .constants import *

import subprocess

def build():
    subprocess.run(f"mkdir build data {Constants.data_folder} {Constants.logs_folder} -p".split())

    # Compile
    result = subprocess.run("meson setup build".split())#, stdout=subprocess.DEVNULL)
    assert result.returncode == 0, "Meson build failed."
    result = subprocess.run("meson compile -C build".split())#, stdout=subprocess.DEVNULL)
    assert result.returncode == 0, "Compilation failed."