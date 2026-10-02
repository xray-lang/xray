cmake_minimum_required(VERSION 3.21)
project(xray_stdlib_source_owner C)
if(NOT CMAKE_GENERATOR STREQUAL "Ninja")
    message(FATAL_ERROR "Source metadata tests require Ninja")
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
get_filename_component(XR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
enable_testing()
set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated/stdlib_embedded_sources.inc")
file(GLOB_RECURSE generator_inputs CONFIGURE_DEPENDS
    "${XR_ROOT}/tools/stdlibgen/*.py" "${XR_ROOT}/stdlib/defs/*.def")
add_custom_command(OUTPUT "${generated}"
    COMMAND ${Python3_EXECUTABLE} -X utf8 "${XR_ROOT}/scripts/generate_stdlib_embedded.py"
        --root "${XR_ROOT}" --source-output "${generated}"
        "${XR_ROOT}/stdlib/io/io.xr" "${XR_ROOT}/stdlib/path/path.xr"
    DEPENDS "${XR_ROOT}/scripts/generate_stdlib_embedded.py" ${generator_inputs}
        "${XR_ROOT}/stdlib/stdlib_boundary.toml"
        "${XR_ROOT}/stdlib/io/io.xr" "${XR_ROOT}/stdlib/path/path.xr"
    VERBATIM)
add_executable(test_stdlib_source_owner ../test_stdlib_source_owner.c
    "${XR_ROOT}/src/module/stdlib_embedded.c" "${XR_ROOT}/src/base/xsha256.c" "${generated}")
target_include_directories(test_stdlib_source_owner PRIVATE "${XR_ROOT}/src" "${CMAKE_CURRENT_BINARY_DIR}/generated")
if(MSVC)
    target_compile_options(test_stdlib_source_owner PRIVATE /W4 /WX /utf-8)
    target_compile_definitions(test_stdlib_source_owner PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
else()
    target_compile_options(test_stdlib_source_owner PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME test_stdlib_source_owner COMMAND ${Python3_EXECUTABLE} -X utf8
    "${CMAKE_CURRENT_LIST_DIR}/../test_stdlib_source_owner.py" $<TARGET_FILE:test_stdlib_source_owner> "${XR_ROOT}")
set_tests_properties(test_stdlib_source_owner PROPERTIES LABELS "unit;stdlib;compiler;budget;ownership" TIMEOUT 30)
