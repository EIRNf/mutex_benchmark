#!/bin/bash
echo $$ | sudo tee /sys/fs/cgroup/a/cgroup.procs

RUN_TIME=20
RUNS=5

## Max Bench
# Node 1 Allocation. DRAM only. Errors.
python3 -m scripts.main 64 $RUN_TIME $RUNS --sweep threads 1 64 4 --capture throughput --bench max -s combined_cxl 
python3 -m scripts.main 64 1 1 --sweep threads 1 64 4 --capture throughput --bench max -s combined_cxl 

cp -r data/generated data/generated_max_cxl_dram_only
# # Adjust CXL lat. Low lat
setpci -v -s 3b:00.1 0xe08.l=0020

# # Node 2 Allocation. CXL backed memory.
python3 -m scripts.main 64 $RUN_TIME $RUNS --sweep threads 1 64 4 --capture throughput --bench max -s combined_cxl --hcxl

cp -r data/generated data/generated_max_cxl_low_lat

# # # Adjust CXL lat. High lat
setpci -v -s 3b:00.1 0xe08.l=ffff

# # # Node 2 Allocation. CXL backed memory.
python3 -m scripts.main 64 $RUN_TIME $RUNS --sweep threads 1 64 4 --capture throughput --bench max -s combined_cxl --hcxl

cp -r data/generated data/generated_max_cxl_high_lat

# ## Min Bench
# # Node 1 Allocation. DRAM only. Errors.
python3 -m scripts.main 64 $RUN_TIME $RUNS --sweep threads 1 64 4 --capture throughput --bench min -s combined_cxl 

cp -r data/generated data/generated_min_cxl_dram_only

# # Adjust CXL lat. Low lat
setpci -v -s 3b:00.1 0xe08.l=0020

# # Node 2 Allocation. CXL backed memory.
python3 -m scripts.main 64 $RUN_TIME $RUNS --sweep threads 1 64 4 --capture throughput --bench min -s combined_cxl --hcxl

cp -r data/generated data/generated_min_cxl_low_lat

# # Adjust CXL lat. High lat
setpci -v -s 3b:00.1 0xe08.l=ffff

# # Node 2 Allocation. CXL backed memory.
python3 -m scripts.main 64 $RUN_TIME $RUNS --sweep threads 1 64 4 --capture throughput --bench min -s combined_cxl --hcxl

cp -r data/generated data/generated_min_cxl_high_lat