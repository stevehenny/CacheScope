add_library(hooks SHARED cs_alloc_hooks.cpp)


target_link_libraries(hooks
  PRIVATE 
    cache_scope_includes
    ${LIBDW_LIBRARIES}
)
target_include_directories(hooks PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)
target_compile_options(hooks PRIVATE
    -fPIC
)

set_target_properties(hooks PROPERTIES
    OUTPUT_NAME "cachescope_alloc"
)
