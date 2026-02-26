add_library(analysis
  Analyzer.cpp
  AnalyzeCommand.cpp
)

target_link_libraries(analysis
  PUBLIC 
  cache_scope_includes
  cachescope_dwarf
  runtime
  ${LIBDW_LIBRARIES}
)
