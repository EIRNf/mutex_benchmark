# LD_PRELOAD / DYLD_INSERT_LIBRARIES shim that injects the repo's
# SoftwareMutex implementations into unmodified pthread applications.
#
# bench_utils.cpp (which carries get_mutex() and, textually, every lock) is
# compiled directly into the shim so the resulting library is self-contained:
# preloading it must not require libutils.{so,dylib} to be resolvable in the
# target process.

cpp = meson.get_compiler('cpp')
shim_dl_dep = cpp.find_library('dl', required : false)
shim_thread_dep = dependency('threads')

mutex_preload_shim = shared_library(
  'mutex_preload',
  utils_core_srcs + files('pthread_lock_shim.cpp'),
  include_directories : [lock_includes, utils_includes],
  dependencies : [boost_dep, nsync_dep, numa_dep, shim_dl_dep, shim_thread_dep],
  cpp_args : ['-fno-strict-aliasing'],
  gnu_symbol_visibility : 'default',
  build_by_default : true,
)
