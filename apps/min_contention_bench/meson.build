link_with = [
  lock_lib,
  utils_lib,
]

include_directories = [
  lock_includes,
  utils_includes,
]

min_contention_bench = executable(
  'min_contention_bench',
  'min_contention_bench.cpp',
  dependencies : [lock_dep, utils_dep, boost_dep, nsync_dep],
)