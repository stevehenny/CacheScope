add_executable(cache_scope main.cpp)

add_subdirectory(common)
add_subdirectory(runtime)
add_subdirectory(platform/linux)
add_subdirectory(dwarf)
add_subdirectory(trace)
add_subdirectory(report)
add_subdirectory(pipeline)
add_subdirectory(analysis)

if (BUILD_TESTING AND CACHESCOPE_BUILD_TESTS)
  add_subdirectory(test)
elseif (CACHESCOPE_BUILD_WORKLOADS)
  add_subdirectory(test EXCLUDE_FROM_ALL)
endif()

if (CACHESCOPE_BUILD_GUI)
  add_subdirectory(gui)
endif()

target_link_libraries(cache_scope
  PRIVATE
    CLI11::CLI11
    cachescope_dwarf
    analysis
    runtime
    platform_linux
    common
    trace
    report
    pipeline
    cache_scope_includes
    cachescope_project_options
    cachescope_warnings
)

if (CACHESCOPE_BUILD_GUI)
  target_link_libraries(cache_scope PRIVATE gui)
  target_compile_definitions(cache_scope PRIVATE CACHESCOPE_BUILD_GUI)
endif()
