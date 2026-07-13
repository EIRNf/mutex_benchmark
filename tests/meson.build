mutex_factory_smoketest = executable(
  'mutex_factory_smoketest',
  'mutex_factory_smoketest.cpp',
  dependencies : [lock_dep, utils_dep, boost_dep, nsync_dep, numa_dep],
)

test('mutex_factory_smoketest', mutex_factory_smoketest)

region_layout_test = executable(
  'region_layout_test',
  'region_layout_test.cpp',
  dependencies : [utils_dep],
)

test('region_layout_test', region_layout_test)

subdir('preload')
