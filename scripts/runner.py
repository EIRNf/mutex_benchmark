# scripts/runner.py

from .constants import Constants
from .logger    import logger
import subprocess
import os

def get_data_file_name(mutex_name, i, **kwargs):
    name_root = f"{Constants.data_folder}/{mutex_name}-{i}-{Constants.bench}"
    if Constants.hardware_cxl:
        name_root += "-hcxl"
    if Constants.software_cxl:
        name_root += "-sxcl"
    for name, value in kwargs.items():
        name_root += f"-{name}={value}"
    name = name_root + ".csv"
    return name

def get_command(mutex_name, *, threads=None, csv=True, thread_level=False, critical_delay=None, noncritical_delay=None, rusage=False):
    if threads is None:
        threads = Constants.bench_n_threads
    if critical_delay is None:
        critical_delay = Constants.critical_delay
    if noncritical_delay is None:
        noncritical_delay = Constants.noncritical_delay
    cmd = [
        Constants.executable, 
        mutex_name, 
        str(threads), 
        str(Constants.bench_n_seconds), 
    ]
    if Constants.software_cxl:
        cmd.insert(0, "sudo")
    if Constants.groups:
        cmd.append(str(Constants.groups))
    if critical_delay != -1:
        cmd += ["--critical-delay", str(critical_delay)]
    if noncritical_delay != -1:
        cmd += ["--noncritical-delay", str(noncritical_delay)]
    if csv:
        cmd.append("--csv")
    if thread_level:
        cmd.append("--thread-level")
    if rusage:
        cmd.append("--rusage")

    if Constants.low_contention:
        cmd.append("--low-contention")
        if Constants.stagger_ms and Constants.stagger_ms > 0:
            cmd += ["--stagger-ms", str(Constants.stagger_ms)]

    logger.debug(f"Bench command: {cmd}")
    return cmd


def _run_command_to_csv(command):
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode != 0:
        stderr = result.stderr.decode("utf-8", errors="replace")
        raise RuntimeError(f"Benchmark command failed: {command}\n{stderr}")
    return result.stdout


def run_experiment_lock_level_single_threaded():
    for i in range(Constants.n_program_iterations):
        for mutex_name in Constants.mutex_names:
            logger.info(f"{mutex_name=} | {i=}")
            data_file_name = get_data_file_name(mutex_name, i)
            if os.path.exists(data_file_name):
                os.remove(data_file_name)
            command = get_command(mutex_name, csv=True, thread_level=False)
            csv_data = _run_command_to_csv(command)
            with open(data_file_name, "wb") as data_file:
                data_file.write(csv_data)


def run_experiment_iter_single_threaded():
    for i in range(Constants.n_program_iterations):
        for iter_variable_value in range(*Constants.iter_range):
            extra_command_args = {Constants.iter_variable_name: iter_variable_value, "rusage":Constants.rusage}
            for mutex_name in Constants.mutex_names:
                logger.info(f"{mutex_name=:<24} | {i=:0>2} | {extra_command_args=}")
                data_file_name = get_data_file_name(mutex_name, i, **extra_command_args)
                if os.path.exists(data_file_name):
                    os.remove(data_file_name)
                command = get_command(mutex_name, csv=True, thread_level=Constants.thread_level, **extra_command_args)
                csv_data = _run_command_to_csv(command)
                with open(data_file_name, "wb") as data_file:
                    data_file.write(csv_data)