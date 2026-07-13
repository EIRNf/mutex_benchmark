

cpp = meson.get_compiler('cpp')
cpp_compiler_version = cpp.version()
message('C++ Compiler Version: ' + cpp_compiler_version)

nsync_dep = cpp.find_library('nsync_cpp', required: false)
boost_dep = dependency('boost', static : true, required: false)
numa_dep = dependency('numa', required: false)

utils_includes = include_directories('.')

# Core sources needed by anything that wants get_mutex() and the lock
# implementations without linking the shared libutils (the LD_PRELOAD shim in
# preload/ compiles these directly into its own .so so it is self-contained).
utils_core_srcs = files('bench_utils.cpp', 'cxl_utils.cpp', 'emucxl_lib.c')

utils_lib = library(
  'utils', utils_core_srcs, 'cxl_utils.hpp',
  'bench_harness.cpp', 'bench_harness.hpp',
  'bench_cli.cpp', 'bench_cli.hpp',
  'emucxl_lib.h', 'emucxl_kernel.h',
  include_directories : utils_includes,
  dependencies : [boost_dep, nsync_dep, numa_dep]
)

utils_dep = declare_dependency(
  include_directories : utils_includes,
  link_with : utils_lib,
)

