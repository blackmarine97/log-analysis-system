cmake_minimum_required(VERSION 3.16)
project(log_analysis_system LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# The Linux server (and its tests) build here. The Windows client has its
# own CMake project under client/ because it is built on a different host.
enable_testing()   # must be in the top-level list for `ctest` to find tests
if(UNIX)
    add_subdirectory(server)
endif()
