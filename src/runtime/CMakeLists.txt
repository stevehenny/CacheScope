add_library(runtime
PipeStream.cpp
FalseSharingAnalysis.cpp
SampleStats.cpp
)

target_link_libraries(runtime
  PUBLIC
    cache_scope_includes
    ${LIBDW_LIBRARIES}
)
