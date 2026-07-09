

cpp = meson.get_compiler('cpp')
cpp_compiler_version = cpp.version()
message('C++ Compiler Version: ' + cpp_compiler_version)

nsync_dep = cpp.find_library('nsync_cpp', required: false)
boost_dep = dependency('boost', static : true, required: false)
numa_dep = dependency('numa', required: false)

utils_includes = include_directories('.')
utils_lib = library(
  'utils', 'bench_utils.cpp', 'cxl_utils.cpp', 'cxl_utils.hpp',
  'bench_harness.cpp', 'bench_harness.hpp',
  'bench_cli.cpp', 'bench_cli.hpp',
  'emucxl_lib.c', 'emucxl_lib.h', 'emucxl_kernel.h',
  include_directories : utils_includes,
  dependencies : [boost_dep, nsync_dep, numa_dep]
)

utils_dep = declare_dependency(
  include_directories : utils_includes,
  link_with : utils_lib,
)

