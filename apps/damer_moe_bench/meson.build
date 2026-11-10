numa_dep = dependency('numa', required : false)

damer_moe_bench = executable(
  'damer_moe_bench',
  'damer_moe_bench.cpp',
  dependencies : [lock_dep, utils_dep, boost_dep, nsync_dep, numa_dep],
)
