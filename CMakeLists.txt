cmake_minimum_required(VERSION 3.12)

project(CacheScope LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBDW REQUIRED libelf)

include_directories(${LIBDW_INCLUDE_DIRS})
link_directories(${LIBDW_LIBRARY_DIRS})

add_subdirectory(src)
add_subdirectory(include)
