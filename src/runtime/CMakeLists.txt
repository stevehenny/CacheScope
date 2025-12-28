add_library(runtime
Tracer.cpp
TracerConfig.cpp
)

target_link_libraries(runtime
  PUBLIC
    cache_scope_includes
    ${LIBDW_LIBRARIES}
)
