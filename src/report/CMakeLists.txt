add_library(report
  JsonReport.cpp
)

target_link_libraries(report
  PUBLIC
    cache_scope_includes
    cachescope_project_options
    cachescope_warnings
    nlohmann_json::nlohmann_json
)
