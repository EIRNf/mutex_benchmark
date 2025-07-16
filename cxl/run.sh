#! /bin/bash

echo "Note: command should be run in a VM simulating a CXL environment"
echo "Note: need to clone emucxl in this directory before running"

# Make kernel module
cd ./emucxl/src
make clean > /dev/null
make > /dev/null 2>/dev/null

# Insert kernel module for CXL
sudo insmod emucxl_kernel.ko

# Run custom tests
gcc ../../test.c emucxl_lib.c emucxl_lib.h \
    && sudo ./a.out