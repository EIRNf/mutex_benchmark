lock_includes = include_directories('.')

# Locks are header/inline-only and aggregated by textual #include into
# lib/utils/bench_utils.cpp's single translation unit (see get_mutex()), so
# there is nothing here to compile into a separate library -- lock_dep only
# needs to carry the header search path.
lock_dep = declare_dependency(
  include_directories : lock_includes,
)
