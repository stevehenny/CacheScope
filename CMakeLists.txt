cmake_minimum_required(VERSION 3.12)

project(CacheScope LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# CLI11
find_package(CLI11 CONFIG REQUIRED)

# libdwarf + libelf
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBDW REQUIRED libdwarf libelf)

add_subdirectory(src)
