# apps/tigon_db_bench/meson.build
#
# Tigon is built with CMake because it is a standalone subproject.
# This custom target wraps apps/tigon_db_bench/build.sh and is build_by_default: false
# so standard meson compiles do not fail on machines without Tigon dependencies.

cmake_prog = find_program('cmake', required : false)
sh_prog = find_program('sh', required : true)

if cmake_prog.found()
  tigon_build_dir = meson.current_build_dir()
  custom_target(
    'tigon_db_bench',
    command : [files('build.sh'), tigon_build_dir],
    output : ['bench_tpcc', 'bench_ycsb'],
    console : true,
    build_by_default : false,
  )
endif
