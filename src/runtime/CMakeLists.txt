add_library(runtime
FalseSharingAnalysis.cpp
SampleStats.cpp
PerfEventRecorder.cpp
CacheTopology.cpp
Thrashing.cpp
)

target_link_libraries(runtime
  PUBLIC
    cache_scope_includes
    ${LIBDW_LIBRARIES}
    ${LIBPFM_LIBRARY}
)
