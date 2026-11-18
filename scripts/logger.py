import os
import logging
import sys


logger = logging.getLogger(__name__)

def init_logger(run):
    """Log to a new file in the run's logs folder and to stdout, at the run's level."""
    logger.setLevel(run.log)
    # Saving data
    log_file_index = 0
    log_file_name = f"{run.logs_folder}/{log_file_index}.log"
    while os.path.isfile(log_file_name):
        log_file_index += 1
        log_file_name = f"{run.logs_folder}/{log_file_index}.log"
    logging.basicConfig(filename=log_file_name, level=logging.DEBUG)
    logger.debug("init_logger finished")

    handler = logging.StreamHandler(sys.stdout)
    handler.setLevel(run.log)
    formatter = logging.Formatter('%(levelname)s:  %(message)s')
    handler.setFormatter(formatter)
    logger.addHandler(handler)