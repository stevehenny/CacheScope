add_library(runtime
Tracer.cpp
Parser.cpp
)

target_link_libraries(runtime
  PUBLIC
    cache_scope_includes
    ${LIBDW_LIBRARIES}
)
