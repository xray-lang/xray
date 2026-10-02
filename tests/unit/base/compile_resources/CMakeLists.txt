cmake_minimum_required(VERSION 3.21)
project(xray_compile_resources_tests LANGUAGES C)

if(NOT CMAKE_GENERATOR STREQUAL "Ninja")
    message(FATAL_ERROR "Compiler resource tests require Ninja")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

enable_testing()
include("${CMAKE_CURRENT_LIST_DIR}/../compile_resources.cmake")
