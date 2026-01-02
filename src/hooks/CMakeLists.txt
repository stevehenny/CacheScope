add_library(heap_hooks SHARED cs_alloc_hooks.cpp )

add_library(stack_hooks SHARED cs_stack_hooks.cpp )

target_link_libraries(heap_hooks
  PRIVATE 
    cache_scope_includes
    ${LIBDW_LIBRARIES}
)
target_include_directories(heap_hooks PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)
target_compile_options(heap_hooks PRIVATE
    -fPIC
)

set_target_properties(heap_hooks PROPERTIES
    OUTPUT_NAME "cachescope_alloc"
)


target_link_libraries(stack_hooks
  PRIVATE 
    cache_scope_includes
    ${LIBDW_LIBRARIES}
)
target_include_directories(stack_hooks PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)
target_compile_options(stack_hooks PRIVATE
    -fPIC
)

set_target_properties(stack_hooks PROPERTIES
    OUTPUT_NAME "cachescope_stack"
)
