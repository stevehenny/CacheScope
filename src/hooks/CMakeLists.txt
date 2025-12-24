add_library(hooks cs_alloc_hooks.cpp)


target_link_libraries(hooks
  PUBLIC
    cache_scope_includes
    ${LIBDW_LIBRARIES}
)
