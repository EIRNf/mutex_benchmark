import os
import logging
import sys

from .constants import *

logger = logging.getLogger(__name__)

def init_logger():    
    # Saving data
    log_file_index = 0
    log_file_name = f"{Constants.logs_folder}/{log_file_index}.log"
    while os.path.isfile(log_file_name):
        log_file_index += 1
        log_file_name = f"{Constants.logs_folder}/{log_file_index}.log"
    logging.basicConfig(filename=log_file_name, level=logging.DEBUG)
    logger.debug("init_logger finished")

    handler = logging.StreamHandler(sys.stdout)
    handler.setLevel(Constants.log)
    formatter = logging.Formatter('%(levelname)s:  %(message)s')
    handler.setFormatter(formatter)
    logger.addHandler(handler)


def log(output):

    with open(log_file_name, "w") as log_file:
        log_file.write(output)