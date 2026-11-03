numa_dep = dependency('numa', required : false)

grouped_contention_bench = executable(
  'grouped_contention_bench',
  'grouped_contention_bench.cpp',
  dependencies : [lock_dep, utils_dep, boost_dep, nsync_dep, numa_dep],
)
