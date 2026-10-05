cmake_minimum_required(VERSION 3.21)
project(xray_compile_owner_tests LANGUAGES C)
if(NOT CMAKE_GENERATOR STREQUAL "Ninja")
    message(FATAL_ERROR "Compiler owner tests require Ninja")
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
get_filename_component(XR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
add_library(compile_owner_core STATIC
    "${XR_ROOT}/src/xir/xxir.c"
    "${XR_ROOT}/src/xir/xxir_verify.c"
    "${XR_ROOT}/src/xir/xxir_defaults.c"
    "${XR_ROOT}/src/xir/xxir_effects.c"
    "${XR_ROOT}/src/xir/xxir_layout.c"
    "${XR_ROOT}/src/xir/xxir_checked.c"
    "${XR_ROOT}/src/xir/xxir_generic.c"
    "${XR_ROOT}/src/xir/xxir_specialize.c"
    "${XR_ROOT}/src/xir/xxir_declarations.c"
    "${XR_ROOT}/src/xir/xxir_types.c"
    "${XR_ROOT}/src/xir/xxir_constraints.c"
    "${XR_ROOT}/src/xir/xxir_constraint_proof.c"
    "${XR_ROOT}/src/xir/xxir_interface.c"
    "${XR_ROOT}/src/xir/xxir_interface_members.c"
    "${XR_ROOT}/src/xir/xxir_implementation.c"
    "${XR_ROOT}/src/xir/xxir_implementation_verify.c"
    "${XR_ROOT}/src/xir/xxir_type_layout.c"
    "${XR_ROOT}/src/xir/xxir_type_inference.c"
    "${XR_ROOT}/src/base/xsha256.c"
)
target_compile_definitions(compile_owner_core PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_include_directories(compile_owner_core PUBLIC "${XR_ROOT}/src")
if(MSVC)
    target_compile_options(compile_owner_core PRIVATE /utf-8 /W4 /WX)
endif()
enable_testing()

add_executable(test_compile_owner test_compile_owner.c)
target_link_libraries(test_compile_owner PRIVATE compile_owner_core)
target_compile_definitions(test_compile_owner PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(test_compile_owner PRIVATE /utf-8 /W4 /WX)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_compile_owner PRIVATE /experimental:c11atomics)
    endif()
endif()
add_test(NAME test_compile_owner COMMAND test_compile_owner)
set_tests_properties(test_compile_owner PROPERTIES LABELS "unit;xir;compiler;ownership;budget" TIMEOUT 120)
include("${CMAKE_CURRENT_LIST_DIR}/../compile_runtime_owner.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../../aot/cgen_compile_resources.cmake")
if(COMMAND xray_stage_windows_asan_runtime)
    xray_stage_windows_asan_runtime("${CMAKE_CURRENT_BINARY_DIR}")
    xray_prepend_windows_asan_runtime_to_directory_tests()
endif()
