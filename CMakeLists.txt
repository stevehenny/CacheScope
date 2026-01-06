cmake_minimum_required(VERSION 3.12)

project(CacheScope LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# CLI11
find_package(CLI11 CONFIG REQUIRED)



add_library(cache_scope_includes INTERFACE)
target_include_directories(cache_scope_includes
  INTERFACE
    ${PROJECT_SOURCE_DIR}/include
    ${LIBELF_INCLUDE_DIRS}
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBELF REQUIRED libelf)

include(CheckSymbolExists)

set(CMAKE_REQUIRED_LIBRARIES dwarf)

check_symbol_exists(dwarf_init_b   "libdwarf.h" HAVE_DWARF_INIT_B)
check_symbol_exists(dwarf_finish   "libdwarf.h" HAVE_DWARF_FINISH_1ARG)

target_compile_definitions(cache_scope_includes
  INTERFACE
    $<$<BOOL:${HAVE_DWARF_INIT_B}>:HAVE_DWARF_INIT_B>
    $<$<BOOL:${HAVE_DWARF_FINISH_1ARG}>:HAVE_DWARF_FINISH_1ARG>
)

add_subdirectory(src)
