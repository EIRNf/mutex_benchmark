

cpp = meson.get_compiler('cpp')
cpp_compiler_version = cpp.version()
message('C++ Compiler Version: ' + cpp_compiler_version)

nsync_dep = cpp.find_library('nsync_cpp', required: false)
boost_dep = dependency('boost', static : true, required: false)

utils_includes = include_directories('.')
utils_lib = library(
  'utils', 'bench_utils.cpp', 'cxl_utils.hpp',
  include_directories : utils_includes,
  dependencies : [boost_dep, nsync_dep]
)

utils_dep = declare_dependency(
  include_directories : utils_includes,
  link_with : utils_lib,
)

