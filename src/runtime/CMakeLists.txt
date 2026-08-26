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
    cachescope_project_options
    cachescope_warnings
    ${LIBDW_LIBRARIES}
    ${LIBPFM_LIBRARY}
)
