add_library(cachescope_dwarf
  DwarfContext.cpp
  Extractor.cpp
)

target_link_libraries(cachescope_dwarf
  PUBLIC
    cache_scope_includes
    dwarf
    elf
)
