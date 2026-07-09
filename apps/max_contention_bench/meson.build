numa_dep = dependency('numa', required : false)

max_contention_bench = executable(
  'max_contention_bench',
  'max_contention_bench.cpp',
  dependencies : [lock_dep, utils_dep, boost_dep, nsync_dep, numa_dep],
)
