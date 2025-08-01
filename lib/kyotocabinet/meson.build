kc_includes = include_directories('.')

kc_lib = library('kc', 

                    ['kccachedb.cc', 'kccompare.cc', 'kccompress.cc', 'kcdb.cc', 'kcdbext.cc', 'kcdirdb.cc', 'kcfile.cc', 'kchashdb.cc', 'kcmap.cc', 'kcplantdb.cc', 'kcpolydb.cc', 'kcprotodb.cc', 'kcregex.cc', 'kcstashdb.cc', 'kctextdb.cc', 'kcthread.cc', 'kcutil.cc', 'kcutiltest.cc'], 
                    dependencies : [boost_dep, nsync_dep],
                    include_directories : [kc_includes], 
                    )


kc_dep = declare_dependency(
  include_directories : kc_includes,
  link_with : kc_lib
)


