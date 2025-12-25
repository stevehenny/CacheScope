add_executable(cache_scope main.cpp)

add_subdirectory(dwarf)
add_subdirectory(analysis)
add_subdirectory(runtime)
add_subdirectory(hooks)
add_subdirectory(test)

target_link_libraries(cache_scope
  PRIVATE
    CLI11::CLI11
    cachescope_dwarf
    analysis
    runtime
    hooks
    cache_scope_includes
)
