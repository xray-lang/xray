# Compiler allocation observation remains independent of runtime domain allocation.
get_filename_component(XIR_OWNER_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
add_executable(test_xir_compile_c_buffer "${CMAKE_CURRENT_LIST_DIR}/test_xir_compile_c_buffer.c")
add_executable(test_xir_compile_api "${CMAKE_CURRENT_LIST_DIR}/test_xir_compile_api.c")
add_executable(test_xir_compile_snapshot_owner
    "${CMAKE_CURRENT_LIST_DIR}/test_xir_compile_snapshot_owner.c"
    "${XIR_OWNER_ROOT}/src/xir/xxir_source_query.c")
target_link_libraries(test_xir_compile_snapshot_owner PRIVATE compile_owner_core)
add_library(xir_compile_owner_runtime STATIC
    "${XIR_OWNER_ROOT}/src/xir/xxir_type_arena.c"
    "${XIR_OWNER_ROOT}/src/xir/xxir_value.c"
    "${XIR_OWNER_ROOT}/src/xir/xxir_scalar.c"
    "${XIR_OWNER_ROOT}/src/xir/xxir_float.c"
    "${XIR_OWNER_ROOT}/src/xir/xxir_output.c"
    "${XIR_OWNER_ROOT}/src/xir/xxir_program.c"
    "${XIR_OWNER_ROOT}/src/xir/xxir_program_match.c"
    "${XIR_OWNER_ROOT}/src/xir/xxir_call.c"
    "${XIR_OWNER_ROOT}/src/xir/xxir_instance.c")
target_link_libraries(xir_compile_owner_runtime PRIVATE compile_owner_core)
add_executable(test_xir_compile_runtime_owner
    "${CMAKE_CURRENT_LIST_DIR}/test_xir_compile_runtime_owner.c"
    "${XIR_OWNER_ROOT}/src/xir/xxir_vm.c")
target_link_libraries(test_xir_compile_runtime_owner PRIVATE xir_compile_owner_runtime)
add_executable(test_xir_compile_emit_owner
    "${CMAKE_CURRENT_LIST_DIR}/test_xir_compile_emit_owner.c"
    "${XIR_OWNER_ROOT}/src/xir/xxir_emit_c.c"
    "${XIR_OWNER_ROOT}/src/aot/xi_cgen_verify_output.c"
    "${XIR_OWNER_ROOT}/src/base/xcompile_resources.c")
target_link_libraries(test_xir_compile_emit_owner PRIVATE xir_compile_owner_runtime)
set(XIR_OWNER_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/compile-owner-generated.c")
add_custom_command(OUTPUT "${XIR_OWNER_GENERATED}"
    COMMAND $<TARGET_FILE:test_xir_compile_emit_owner> "${XIR_OWNER_GENERATED}"
    DEPENDS test_xir_compile_emit_owner
    VERBATIM)
add_executable(test_xir_compile_native_owner
    "${CMAKE_CURRENT_LIST_DIR}/test_xir_compile_native_owner.c"
    "${XIR_OWNER_ROOT}/src/base/xcompile_resources.c" "${XIR_OWNER_GENERATED}")
target_link_libraries(test_xir_compile_native_owner PRIVATE xir_compile_owner_runtime)
if(DEFINED XR_XIR_OLD_ADMISSION_OBJECT)
    if(NOT EXISTS "${XR_XIR_OLD_ADMISSION_OBJECT}")
        message(FATAL_ERROR "The independent old ABI object is required")
    endif()
    add_executable(test_xir_compile_old_data
        "${CMAKE_CURRENT_LIST_DIR}/test_xir_compile_old_data.c"
        "${XIR_OWNER_ROOT}/src/base/xcompile_resources.c" "${XR_XIR_OLD_ADMISSION_OBJECT}")
    target_link_libraries(test_xir_compile_old_data PRIVATE xir_compile_owner_runtime)
    target_include_directories(test_xir_compile_old_data PRIVATE "${XIR_OWNER_ROOT}/src")
    target_compile_features(test_xir_compile_old_data PRIVATE c_std_11)
    if(MSVC)
        target_compile_options(test_xir_compile_old_data PRIVATE /W4 /WX /utf-8)
        target_compile_definitions(test_xir_compile_old_data PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
        if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
            target_compile_options(test_xir_compile_old_data PRIVATE /experimental:c11atomics)
        endif()
    endif()
    add_test(NAME test_xir_compile_old_data COMMAND test_xir_compile_old_data)
    set_tests_properties(test_xir_compile_old_data PROPERTIES LABELS "unit;xir;abi" TIMEOUT 30)
endif()
foreach(owner_target xir_compile_owner_runtime test_xir_compile_snapshot_owner test_xir_compile_api test_xir_compile_c_buffer test_xir_compile_runtime_owner
    test_xir_compile_emit_owner test_xir_compile_native_owner)
    target_include_directories(${owner_target} PRIVATE "${XIR_OWNER_ROOT}/src" "${XIR_OWNER_ROOT}/include")
    target_compile_features(${owner_target} PRIVATE c_std_11)
    if(MSVC)
        target_compile_definitions(${owner_target} PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
        target_compile_options(${owner_target} PRIVATE /W4 /WX /utf-8)
        if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
            target_compile_options(${owner_target} PRIVATE /experimental:c11atomics)
        endif()
    else()
        target_compile_options(${owner_target} PRIVATE -Wall -Wextra -Werror -pedantic)
    endif()
endforeach()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
if(MSVC)
    set(XIR_OWNER_MSVC 1)
else()
    set(XIR_OWNER_MSVC 0)
endif()
add_test(NAME test_xir_compile_api_rejection
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/check_compile_api_rejection.py"
        --compiler "${CMAKE_C_COMPILER}" --msvc "${XIR_OWNER_MSVC}" --root "${XIR_OWNER_ROOT}"
        --output "${CMAKE_CURRENT_BINARY_DIR}/compile-api-rejections")
set_tests_properties(test_xir_compile_api_rejection PROPERTIES LABELS "unit;xir;abi" TIMEOUT 60)
foreach(owner_target test_xir_compile_snapshot_owner test_xir_compile_api test_xir_compile_c_buffer test_xir_compile_runtime_owner test_xir_compile_native_owner)
    add_test(NAME ${owner_target} COMMAND ${owner_target})
    set_tests_properties(${owner_target} PROPERTIES LABELS "unit;xir;compiler;ownership;budget;abi" TIMEOUT 60)
endforeach()
