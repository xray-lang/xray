cmake_minimum_required(VERSION 3.21)
project(xray_compile_resources_tests LANGUAGES C)

if(NOT CMAKE_GENERATOR STREQUAL "Ninja")
    message(FATAL_ERROR "Compiler resource tests require Ninja")
endif()

get_filename_component(XR_RESOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Compile the ordinary translation unit separately from allocation fault injection.
add_library(compile_resources_production OBJECT "${XR_RESOURCE_ROOT}/src/base/xcompile_resources.c")
add_executable(test_compile_resources "${XR_RESOURCE_ROOT}/tests/unit/base/test_compile_resources.c")
target_include_directories(test_compile_resources PRIVATE "${XR_RESOURCE_ROOT}/src")
if(MSVC)
    target_compile_definitions(compile_resources_production PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(test_compile_resources PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(compile_resources_production PRIVATE /W4 /WX /utf-8)
    target_compile_options(test_compile_resources PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(compile_resources_production PRIVATE -Wall -Wextra -Werror -pedantic)
    target_compile_options(test_compile_resources PRIVATE -Wall -Wextra -Werror -pedantic)
endif()

enable_testing()
add_test(NAME test_compile_resources COMMAND test_compile_resources)
