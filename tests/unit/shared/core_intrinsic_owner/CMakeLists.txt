cmake_minimum_required(VERSION 3.21)
project(xray_core_intrinsic_owner_tests LANGUAGES C)
if(NOT CMAKE_GENERATOR STREQUAL "Ninja")
    message(FATAL_ERROR "Core intrinsic owner tests require Ninja")
endif()
get_filename_component(INTRINSIC_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
add_executable(test_core_intrinsic_owner test_core_intrinsic_owner.c
    "${INTRINSIC_ROOT}/src/shared/xr_core_intrinsic_registry.c"
    "${INTRINSIC_ROOT}/src/base/xcompile_resources.c")
target_include_directories(test_core_intrinsic_owner PRIVATE "${INTRINSIC_ROOT}/src")
set_target_properties(test_core_intrinsic_owner PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_definitions(test_core_intrinsic_owner PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(test_core_intrinsic_owner PRIVATE /W4 /WX /utf-8)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_core_intrinsic_owner PRIVATE /experimental:c11atomics)
    endif()
else()
    target_compile_options(test_core_intrinsic_owner PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
enable_testing()
add_test(NAME test_core_intrinsic_owner COMMAND test_core_intrinsic_owner)
set_tests_properties(test_core_intrinsic_owner PROPERTIES
    LABELS "unit;compiler;budget" TIMEOUT 30)
