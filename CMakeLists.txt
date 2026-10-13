cmake_minimum_required(VERSION 3.20)

project(eagls VERSION 0.1.0 LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "请使用独立构建目录，例如 cmake -S . -B build/local。")
endif()

option(EAGLS_BUILD_CLI "构建命令行程序" ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

include(cmake/ProjectOptions.cmake)
include(CTest)

configure_file(
    cmake/version_config.hpp.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/eagls/version_config.hpp"
    @ONLY
)

add_subdirectory(src)

if(EAGLS_BUILD_CLI)
    add_subdirectory(apps/cli)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
