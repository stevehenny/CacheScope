add_library(common
  Utils.cpp
)

target_link_libraries(common
  PUBLIC
    cache_scope_includes
    ${LIBDW_LIBRARIES}
    ${LIBPFM_LIBRARY}
)
