link_with = [
  kc_lib,
  lock_lib,
  utils_lib,
]

include_directories = [
  kc_includes,
  lock_includes,
  utils_includes,
]

kc_cache_bench = executable(
  'kc_cache_bench',
  'kc_cache_bench.cpp',
  dependencies : [ kc_dep, lock_dep, utils_dep, boost_dep, nsync_dep],
)