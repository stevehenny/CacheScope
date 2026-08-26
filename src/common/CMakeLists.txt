add_library(common
  Utils.cpp
)

target_link_libraries(common
  PUBLIC
    cache_scope_includes
    cachescope_project_options
    cachescope_warnings
    ${LIBDW_LIBRARIES}
    ${LIBPFM_LIBRARY}
)
