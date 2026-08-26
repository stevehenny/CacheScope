add_library(trace
  Trace.cpp
)

target_link_libraries(trace
  PUBLIC
    cache_scope_includes
    cachescope_project_options
    cachescope_warnings
    nlohmann_json::nlohmann_json
)
