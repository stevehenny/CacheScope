add_library(analysis Analyzer.cpp)

target_link_libraries(analysis
  PUBLIC 
  cache_scope_includes
  ${LIBDW_LIBRARIES}
)
