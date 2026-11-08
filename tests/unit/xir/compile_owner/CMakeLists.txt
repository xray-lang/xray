cmake_minimum_required(VERSION 3.21)
project(xray_compile_owner_tests LANGUAGES C)
if(NOT CMAKE_GENERATOR STREQUAL "Ninja")
    message(FATAL_ERROR "Compiler owner tests require Ninja")
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
get_filename_component(XR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
if(WIN32)
    set(XR_COMPILE_OWNER_CLOCK "${XR_ROOT}/src/os/win/time_win.c")
else()
    set(XR_COMPILE_OWNER_CLOCK "${XR_ROOT}/src/os/unix/time_unix.c")
endif()
add_library(compile_owner_core STATIC
    "${XR_ROOT}/src/base/xcompile_request.c"
    "${XR_COMPILE_OWNER_CLOCK}"
    "${XR_ROOT}/src/xir/xxir.c"
    "${XR_ROOT}/src/xir/xxir_input_owned.c"
    "${XR_ROOT}/src/xir/xxir_verify.c"
    "${XR_ROOT}/src/xir/xxir_defaults.c"
    "${XR_ROOT}/src/xir/xxir_effects.c"
    "${XR_ROOT}/src/xir/xxir_storage_flow.c"
    "${XR_ROOT}/src/hir/xhir_escape.c"
    "${XR_ROOT}/src/xir/xxir_layout.c"
    "${XR_ROOT}/src/xir/xxir_checked.c"
    "${XR_ROOT}/src/xir/xxir_generic.c"
    "${XR_ROOT}/src/xir/xxir_specialize.c"
    "${XR_ROOT}/src/xir/xxir_declarations.c"
    "${XR_ROOT}/src/xir/xxir_types.c"
    "${XR_ROOT}/src/xir/xxir_view_affine.c"
    "${XR_ROOT}/src/xir/xxir_constraints.c"
    "${XR_ROOT}/src/xir/xxir_constraint_proof.c"
    "${XR_ROOT}/src/xir/xxir_interface.c"
    "${XR_ROOT}/src/xir/xxir_interface_members.c"
    "${XR_ROOT}/src/xir/xxir_implementation.c"
    "${XR_ROOT}/src/xir/xxir_implementation_verify.c"
    "${XR_ROOT}/src/xir/xxir_type_layout.c"
    "${XR_ROOT}/src/xir/xxir_type_inference.c"
    "${XR_ROOT}/src/shared/xnative_declaration.c"
    "${XR_ROOT}/src/base/xsha256.c"
)
target_compile_definitions(compile_owner_core PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_include_directories(compile_owner_core PUBLIC "${XR_ROOT}/src")
if(MSVC)
    target_compile_options(compile_owner_core PRIVATE /utf-8 /W4 /WX)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(compile_owner_core PRIVATE /experimental:c11atomics)
    endif()
endif()
enable_testing()

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME test_root_construction72_vectors
    COMMAND "${Python3_EXECUTABLE}" "${XR_ROOT}/tests/unit/xir/derive_combined_wire.py"
        --source-root "${XR_ROOT}" --output-root "${XR_ROOT}")
set_tests_properties(test_root_construction72_vectors PROPERTIES
    LABELS "unit;xir;compiler;wire;independent;retired" TIMEOUT 120)
add_test(NAME test_root_construction75_vectors
    COMMAND "${Python3_EXECUTABLE}" "${XR_ROOT}/tests/unit/xir/derive_root_comptime75_vectors.py"
        --source-root "${XR_ROOT}" --output-root "${XR_ROOT}")
set_tests_properties(test_root_construction75_vectors PROPERTIES
    LABELS "unit;xir;compiler;wire;independent" TIMEOUT 120)
include("${CMAKE_CURRENT_LIST_DIR}/../compile_runtime_owner.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../../aot/cgen_compile_resources.cmake")
if(COMMAND xray_stage_windows_asan_runtime)
    xray_stage_windows_asan_runtime("${CMAKE_CURRENT_BINARY_DIR}")
    xray_prepend_windows_asan_runtime_to_directory_tests()
endif()
