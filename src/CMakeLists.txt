add_executable(cache_scope main.cpp)

add_subdirectory(static)

target_include_directories(cache_scope
  PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src/static
    ${PROJECT_SOURCE_DIR}/src/dynamic
)

target_link_libraries(cache_scope
  PRIVATE
    CLI11::CLI11
    static_analysis
    ${LIBDW_LIBRARIES}
)
