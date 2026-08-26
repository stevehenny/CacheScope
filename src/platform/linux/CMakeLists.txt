add_library(platform_linux
  Doctor.cpp
)

target_link_libraries(platform_linux
  PUBLIC
    cache_scope_includes
    cachescope_project_options
    cachescope_warnings
    runtime
)
