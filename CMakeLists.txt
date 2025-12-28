cmake_minimum_required(VERSION 3.12)

project(CacheScope LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Debug)

# Enable generation of compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Compiler flags for debug builds (frame pointer + instrumentation)
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fno-omit-frame-pointer -finstrument-functions")

# CLI11
find_package(CLI11 CONFIG REQUIRED)

# libdwarf + libelf
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBDW REQUIRED libdwarf libelf)

add_library(cache_scope_includes INTERFACE)
target_include_directories(cache_scope_includes
  INTERFACE
    ${PROJECT_SOURCE_DIR}/include
)

add_subdirectory(src)
