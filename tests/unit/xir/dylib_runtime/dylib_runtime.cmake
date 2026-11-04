set(dylib_runtime_dir "${CMAKE_CURRENT_LIST_DIR}")
set(dylib_runtime_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/dylib-runtime")
file(MAKE_DIRECTORY "${dylib_runtime_generated}")

add_library(test_xir_dylib_data SHARED "${dylib_runtime_dir}/test_dylib_data.c")
add_executable(test_xir_dylib_source "${dylib_runtime_dir}/test_dylib_source.c")
target_link_libraries(test_xir_dylib_source PRIVATE
    xray_xir_source xray_xir_vm xray_xir_cgen xray_xir_ctfe xray_xir_runtime_host)
add_dependencies(test_xir_dylib_source test_xir_dylib_data)
add_custom_command(OUTPUT "${dylib_runtime_generated}/dylib_runtime.c"
    COMMAND $<TARGET_FILE:test_xir_dylib_source> --emit "${dylib_runtime_generated}"
    DEPENDS test_xir_dylib_source
        "${dylib_runtime_dir}/dylib_cases.h"
        "${dylib_runtime_dir}/dylib_runtime.h"
        "${PROJECT_SOURCE_DIR}/stdlib/sys/sys.xr"
        "${PROJECT_SOURCE_DIR}/stdlib/path/path.xr"
    VERBATIM)
add_executable(test_xir_dylib_native "${dylib_runtime_dir}/test_dylib_native.c"
    "${dylib_runtime_generated}/dylib_runtime.c")
target_link_libraries(test_xir_dylib_native PRIVATE xray_xir_scalar)
add_dependencies(test_xir_dylib_native test_xir_dylib_data)
foreach(target test_xir_dylib_source test_xir_dylib_native)
    target_compile_definitions(${target} PRIVATE
        XR_DYLIB_ROOT="${PROJECT_SOURCE_DIR}"
        XR_DYLIB_GENERATED="${dylib_runtime_generated}"
        XR_DYLIB_DATA="$<TARGET_FILE:test_xir_dylib_data>")
    if(WIN32)
        add_test(NAME ${target} COMMAND ${target})
        set_tests_properties(${target} PROPERTIES RUN_SERIAL TRUE TIMEOUT 120
            LABELS "unit;xir;dylib;host;ownership;runtime")
    endif()
endforeach()
foreach(target test_xir_dylib_data test_xir_dylib_source test_xir_dylib_native)
    target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/src")
    set_target_properties(${target} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
        if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
            target_compile_options(${target} PRIVATE /experimental:c11atomics)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -pedantic)
        target_compile_definitions(${target} PRIVATE _POSIX_C_SOURCE=200809L)
    endif()
endforeach()
