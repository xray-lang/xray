cmake_minimum_required(VERSION 3.25)
project(NativeInvocationOwner C)
if(NOT WIN32 OR NOT CMAKE_GENERATOR STREQUAL "Ninja")
    message(FATAL_ERROR "This gate requires Windows Ninja")
endif()
get_filename_component(XR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
if(NOT IS_ABSOLUTE "${INVOCATION_SOURCE_BUILD}")
    message(FATAL_ERROR "Set INVOCATION_SOURCE_BUILD to the qualified Source archive tree")
endif()
if(NOT INVOCATION_SDK)
    set(INVOCATION_SDK "${XR_ROOT}/build/xir-runtime-sdk")
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
add_compile_options(/W4 /WX /utf-8)
add_compile_definitions(NDEBUG WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    add_compile_options(/experimental:c11atomics)
endif()
include_directories("${XR_ROOT}/src" "${XR_ROOT}/include" "${INVOCATION_SDK}")
set(source_libraries)
foreach(item xir/xray_xir_source_product xir/xray_xir_source xir/xray_xir_vm
    xir/xray_xir_scalar xir/xray_xir_cgen xir/xray_xir xir/xray_xir_admission
    xir/xray_xir_declarations xir/xray_compiler_parser base/xray_compile_resources)
    get_filename_component(name "${item}" NAME)
    set(path "${INVOCATION_SOURCE_BUILD}/src/${item}.lib")
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Missing Source archive: ${path}")
    endif()
    add_library(${name} STATIC IMPORTED)
    set_target_properties(${name} PROPERTIES IMPORTED_LOCATION "${path}")
    list(APPEND source_libraries ${name})
endforeach()
include("${XR_ROOT}/src/app/toolchain/xir_process.cmake")
add_library(invocation_production STATIC
    "${XR_ROOT}/src/app/toolchain/xtc_xir_invocation.c"
    "${XR_ROOT}/src/app/toolchain/xtc_xir_namespace.c"
    "${XR_ROOT}/src/app/toolchain/xtc_dependencies.c"
    "${XR_ROOT}/src/app/toolchain/xtc_xir_target.c"
    "${XR_ROOT}/src/app/toolchain/xtc_xir_sysroot.c"
    "${XR_ROOT}/src/app/toolchain/xtc_xir_images.c"
    "${XR_ROOT}/src/base/xjson_cursor.c"
    "${XR_ROOT}/src/toolchain/xr_xir_runtime_sdk.c"
    "${XR_ROOT}/src/toolchain/xr_toolchain_binding.c"
    "${XR_ROOT}/src/aot/program/xr_native_artifact.c"
    "${XR_ROOT}/src/aot/program/xr_xir_native_projection.c")
target_link_libraries(invocation_production PUBLIC xray_toolchain_process ${source_libraries} bcrypt)
add_executable(test_native_invocation_owner test_native_invocation_owner.c)
target_link_libraries(test_native_invocation_owner PRIVATE invocation_production)
add_executable(test_native_invocation_faults test_native_invocation_owner.c)
target_compile_definitions(test_native_invocation_faults PRIVATE INVOCATION_INJECTED)
target_link_libraries(test_native_invocation_faults PRIVATE invocation_production)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
find_program(INVOCATION_MSVC NAMES cl REQUIRED)
find_program(INVOCATION_LINKER NAMES link REQUIRED)
enable_testing()
add_test(NAME native_invocation_owner COMMAND "${Python3_EXECUTABLE}" -X utf8
    "${CMAKE_CURRENT_LIST_DIR}/test_native_invocation_owner.py" $<TARGET_FILE:test_native_invocation_owner>
    "${XR_ROOT}" "${INVOCATION_MSVC}" "${INVOCATION_LINKER}" "${INVOCATION_SDK}")
add_test(NAME native_invocation_faults COMMAND "${Python3_EXECUTABLE}" -X utf8
    "${CMAKE_CURRENT_LIST_DIR}/test_native_invocation_owner.py" $<TARGET_FILE:test_native_invocation_faults>
    "${XR_ROOT}" "${INVOCATION_MSVC}" "${INVOCATION_LINKER}" "${INVOCATION_SDK}" "faults")
add_test(NAME native_invocation_guard COMMAND "${Python3_EXECUTABLE}" -X utf8
    "${CMAKE_CURRENT_LIST_DIR}/test_native_invocation_owner.py" $<TARGET_FILE:test_native_invocation_faults>
    "${XR_ROOT}" "${INVOCATION_MSVC}" "${INVOCATION_LINKER}" "${INVOCATION_SDK}" "guard")
set_tests_properties(native_invocation_owner PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE)
set_tests_properties(native_invocation_faults PROPERTIES TIMEOUT 1200 RUN_SERIAL TRUE)
set_tests_properties(native_invocation_guard PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE)
