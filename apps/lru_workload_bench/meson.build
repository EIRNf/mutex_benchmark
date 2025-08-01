link_with = [
  lock_lib,
  utils_lib,
]

include_directories = [
  lock_includes,
  utils_includes,
]

lru_workload_bench = executable(
  'lru_workload_bench',
  'lru_workload_bench.cpp',
  dependencies : [lock_dep, utils_dep, boost_dep, nsync_dep],
)