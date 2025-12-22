add_executable(cache_scope main.cpp)

add_subdirectory(dwarf)
add_subdirectory(analysis)

target_link_libraries(cache_scope
  PRIVATE
    CLI11::CLI11
    cachescope_dwarf
    analysis
    cache_scope_includes
)
