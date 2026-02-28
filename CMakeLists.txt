cmake_minimum_required(VERSION 3.12)

project(CacheScope LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Release)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CACHESCOPE_BUILD_GUI "Build CacheScope GUI" ON)

# CLI11
find_package(CLI11 CONFIG REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBELF REQUIRED libelf)

find_path(LIBPFM_INCLUDE_DIR NAMES perfmon/pfmlib.h)
find_library(LIBPFM_LIBRARY NAMES pfm)
if (NOT LIBPFM_INCLUDE_DIR OR NOT LIBPFM_LIBRARY)
  message(FATAL_ERROR "libpfm not found (need perfmon/pfmlib.h and -lpfm)")
endif()

add_library(cache_scope_includes INTERFACE)
target_include_directories(cache_scope_includes
  INTERFACE
    ${PROJECT_SOURCE_DIR}/include
    ${LIBELF_INCLUDE_DIRS}
    ${LIBPFM_INCLUDE_DIR}
)

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

include(InstallRequiredSystemLibraries)

set(CPACK_PACKAGE_NAME "cachescope")
set(CPACK_PACKAGE_VERSION "0.0.0")
set(CPACK_GENERATOR "DEB;RPM")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Stephen Henstrom")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6 (>=2.31)")

include(CPack)
install(TARGETS cache_scope DESTINATION bin)
