add_executable(cache_scope main.cpp)

target_include_directories(cache_scope
  PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/include/CLI/include/CLI
)

target_link_libraries(cache_scope CLI)
