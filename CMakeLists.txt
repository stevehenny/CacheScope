cmake_minimum_required(VERSION 3.22)

project(CacheScope VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CACHESCOPE_BUILD_GUI "Build CacheScope GUI" ON)
option(CACHESCOPE_BUILD_TESTS "Build CacheScope deterministic tests" ON)
option(CACHESCOPE_BUILD_WORKLOADS "Build manual profiling workloads" ON)
option(CACHESCOPE_WARNINGS_AS_ERRORS "Treat CacheScope warnings as errors" OFF)
option(CACHESCOPE_ENABLE_PACKAGING "Enable experimental packages" OFF)

include(CTest)
include(GNUInstallDirs)

find_package(CLI11 CONFIG REQUIRED)

find_package(nlohmann_json 3.11 QUIET)
if (NOT nlohmann_json_FOUND)
  include(FetchContent)
  FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(nlohmann_json)
endif()

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

add_library(cachescope_project_options INTERFACE)
target_compile_features(cachescope_project_options INTERFACE cxx_std_20)

add_library(cachescope_warnings INTERFACE)
target_compile_options(cachescope_warnings INTERFACE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
  $<$<AND:$<BOOL:${CACHESCOPE_WARNINGS_AS_ERRORS}>,$<CXX_COMPILER_ID:GNU,Clang>>:-Werror>
)

include(CheckSymbolExists)
set(CMAKE_REQUIRED_LIBRARIES dwarf)
check_symbol_exists(dwarf_init_b "libdwarf.h" HAVE_DWARF_INIT_B)
check_symbol_exists(dwarf_finish "libdwarf.h" HAVE_DWARF_FINISH_1ARG)

target_compile_definitions(cache_scope_includes
  INTERFACE
    CACHESCOPE_VERSION="${PROJECT_VERSION}"
    $<$<BOOL:${HAVE_DWARF_INIT_B}>:HAVE_DWARF_INIT_B>
    $<$<BOOL:${HAVE_DWARF_FINISH_1ARG}>:HAVE_DWARF_FINISH_1ARG>
)

add_subdirectory(src)

install(TARGETS cache_scope RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if (CACHESCOPE_ENABLE_PACKAGING)
  include(InstallRequiredSystemLibraries)
  set(CPACK_PACKAGE_NAME "cachescope")
  set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
  set(CPACK_GENERATOR "DEB")
  set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Stephen Henstrom")
  set(CPACK_DEBIAN_PACKAGE_DEPENDS
      "libc6 (>= 2.35), libstdc++6, libdwarf1, libelf1, libpfm4")
  include(CPack)
endif()
