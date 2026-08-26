add_library(pipeline
  AnalysisPipeline.cpp
)

target_link_libraries(pipeline
  PUBLIC
    cache_scope_includes
    cachescope_project_options
    cachescope_warnings
    report
    runtime
    trace
)
