add_executable(cache_scope main.cpp)

add_subdirectory(dwarf)
add_subdirectory(analysis)
add_subdirectory(runtime)
add_subdirectory(test)
if (CACHESCOPE_BUILD_GUI)
  add_subdirectory(gui)
endif()

target_link_libraries(cache_scope
  PRIVATE
    CLI11::CLI11
    cachescope_dwarf
    analysis
    runtime
    cache_scope_includes
)

if (CACHESCOPE_BUILD_GUI)
  target_link_libraries(cache_scope PRIVATE gui)
  target_compile_definitions(cache_scope PRIVATE CACHESCOPE_BUILD_GUI)
endif()
