set(pipe_runtime_dir "${CMAKE_CURRENT_LIST_DIR}")
set(pipe_runtime_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/pipe-runtime")
file(MAKE_DIRECTORY "${pipe_runtime_generated}")

add_executable(test_xir_pipe_source "${pipe_runtime_dir}/test_pipe_source.c")
target_link_libraries(test_xir_pipe_source PRIVATE
    xray_xir_source xray_xir_vm xray_xir_cgen xray_xir_ctfe xray_xir_runtime_host)
target_compile_definitions(test_xir_pipe_source PRIVATE
    XR_PIPE_GENERATED="${pipe_runtime_generated}")
add_custom_command(OUTPUT "${pipe_runtime_generated}/pipe_runtime.c"
    COMMAND $<TARGET_FILE:test_xir_pipe_source> --emit "${pipe_runtime_generated}"
    DEPENDS test_xir_pipe_source
        "${pipe_runtime_dir}/pipe_cases.h"
        "${pipe_runtime_dir}/pipe_runtime.h"
        "${PROJECT_SOURCE_DIR}/stdlib/sys/sys.xr"
        "${PROJECT_SOURCE_DIR}/stdlib/path/path.xr"
        "${PROJECT_SOURCE_DIR}/tests/unit/xir/xir_genuine_sys_path.h"
    VERBATIM)

add_executable(test_xir_pipe_native "${pipe_runtime_dir}/test_pipe_native.c"
    "${pipe_runtime_generated}/pipe_runtime.c")
target_link_libraries(test_xir_pipe_native PRIVATE xray_xir_scalar)
foreach(target test_xir_pipe_source test_xir_pipe_native)
    target_compile_definitions(${target} PRIVATE XR_PIPE_ROOT="${PROJECT_SOURCE_DIR}")
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
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES RUN_SERIAL TRUE TIMEOUT 120
        LABELS "unit;xir;pipe;host;ownership;runtime")
endforeach()
