

cpp = meson.get_compiler('cpp')

nsync_dep = cpp.find_library('nsync_cpp')

boost_dep = dependency('boost', static : true)

utils_includes = include_directories('.')
utils_lib = library(
  'utils', 'bench_utils.cpp', 
  include_directories : utils_includes,
  dependencies : [boost_dep, nsync_dep]
)

utils_dep = declare_dependency(
  include_directories : utils_includes,
  link_with : utils_lib,
)

