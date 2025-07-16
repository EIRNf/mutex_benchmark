lock_includes = include_directories('.')

cpp = meson.get_compiler('cpp')

nsync_dep = cpp.find_library('nsync_cpp')

boost_dep = dependency('boost', static : true)

lock_lib = library('lock', 
                    ['pthread_lock.cpp', 'boost_lock.cpp', 'nsync_lock.cpp', 'spin_lock.hpp', 'dijkstra_lock.cpp', 'cpp_std_mutex.cpp', 'bakery_mutex.cpp', 'lamport_lock.cpp', 'bakery_nonatomic_mutex.cpp'], 
                    dependencies : [boost_dep, nsync_dep],
                    include_directories : [lock_includes], 
                    )


lock_dep = declare_dependency(
  include_directories : lock_includes,
  link_with : lock_lib
)

