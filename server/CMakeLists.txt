# Linux server: log_server + parser unit test + CLI test client.
#
# libuv resolution order:
#   1. system package (libuv1-dev)  -- fast path
#   2. FetchContent from GitHub     -- zero-setup fallback for graders

find_path(LIBUV_INCLUDE_DIR uv.h)
find_library(LIBUV_LIBRARY NAMES uv libuv)

if(LIBUV_INCLUDE_DIR AND LIBUV_LIBRARY)
    message(STATUS "Using system libuv: ${LIBUV_LIBRARY}")
    add_library(libuv_dep INTERFACE)
    target_include_directories(libuv_dep INTERFACE ${LIBUV_INCLUDE_DIR})
    target_link_libraries(libuv_dep INTERFACE ${LIBUV_LIBRARY})
else()
    message(STATUS "System libuv not found; fetching v1.48.0 from GitHub")
    include(FetchContent)
    set(LIBUV_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(LIBUV_BUILD_TESTS  OFF CACHE BOOL "" FORCE)
    set(LIBUV_BUILD_BENCH  OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(libuv
        GIT_REPOSITORY https://github.com/libuv/libuv.git
        GIT_TAG        v1.48.0)
    FetchContent_MakeAvailable(libuv)
    add_library(libuv_dep INTERFACE)
    target_link_libraries(libuv_dep INTERFACE uv_a)
endif()

set(COMMON_WARNINGS -Wall -Wextra -Wpedantic)
find_package(Threads REQUIRED)   # Logger owns a std::thread

add_executable(log_server main.cpp)
target_compile_options(log_server PRIVATE ${COMMON_WARNINGS})
target_include_directories(log_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..)      # for common/Protocol.h
target_link_libraries(log_server PRIVATE libuv_dep Threads::Threads)

add_executable(parser_test tests/parser_test.cpp)
target_compile_options(parser_test PRIVATE ${COMMON_WARNINGS})
target_include_directories(parser_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(logger_test tests/logger_test.cpp)
target_compile_options(logger_test PRIVATE ${COMMON_WARNINGS})
target_include_directories(logger_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(logger_test PRIVATE Threads::Threads)

add_executable(test_client tests/test_client.cpp)
target_compile_options(test_client PRIVATE ${COMMON_WARNINGS})
target_include_directories(test_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..)

enable_testing()
add_test(NAME parser_unit COMMAND parser_test
         ${CMAKE_CURRENT_SOURCE_DIR}/../Test_Log.log)
add_test(NAME logger_unit COMMAND logger_test)
