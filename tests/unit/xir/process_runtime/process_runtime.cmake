set(process_runtime_dir "${CMAKE_CURRENT_LIST_DIR}")
set(process_runtime_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/process-runtime")
file(MAKE_DIRECTORY "${process_runtime_generated}")

add_executable(test_xir_process_source "${process_runtime_dir}/test_process_source.c")
target_link_libraries(test_xir_process_source PRIVATE
    xray_xir_source xray_xir_vm xray_xir_cgen xray_xir_ctfe xray_xir_runtime_host)
target_compile_definitions(test_xir_process_source PRIVATE
    XR_PROCESS_GENERATED="${process_runtime_generated}")
add_custom_command(OUTPUT "${process_runtime_generated}/process_runtime.c"
    COMMAND $<TARGET_FILE:test_xir_process_source> --emit "${process_runtime_generated}"
    DEPENDS test_xir_process_source
        "${process_runtime_dir}/process_cases.h"
        "${process_runtime_dir}/process_runtime.h"
        "${PROJECT_SOURCE_DIR}/stdlib/sys/sys.xr"
        "${PROJECT_SOURCE_DIR}/stdlib/path/path.xr"
        "${PROJECT_SOURCE_DIR}/stdlib/time/time.xr"
    VERBATIM)

add_executable(test_xir_process_native "${process_runtime_dir}/test_process_native.c"
    "${process_runtime_generated}/process_runtime.c")
target_link_libraries(test_xir_process_native PRIVATE xray_xir_scalar)
foreach(target test_xir_process_source test_xir_process_native)
    target_compile_definitions(${target} PRIVATE XR_PROCESS_ROOT="${PROJECT_SOURCE_DIR}")
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
    if(WIN32)
        add_test(NAME ${target} COMMAND ${target})
        set_tests_properties(${target} PROPERTIES RUN_SERIAL TRUE TIMEOUT 120
            LABELS "unit;xir;process;host;ownership;runtime")
    endif()
endforeach()
