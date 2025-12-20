add_executable(cache_scope main.cpp)

target_include_directories(cache_scope
  PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/include/CLI11/include/CLI
    ${PROJECT_SOURCE_DIR}/src/static
    ${PROJECT_SOURCE_DIR}/include/libdwarf/src/lib/libdwarf
)
add_subdirectory(static)
target_link_libraries(cache_scope CLI11 static_analysis ${LIBDW_LIBRARIES} dwarf)
