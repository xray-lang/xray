set(host_os_dir "${CMAKE_CURRENT_LIST_DIR}")
set(host_os_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/host-os")
file(MAKE_DIRECTORY "${host_os_generated}")
set(host_os_c
    "${host_os_generated}/host_count.c"
    "${host_os_generated}/host_pin.c"
    "${host_os_generated}/host_path.c"
    "${host_os_generated}/host_system.c"
    "${host_os_generated}/host_stat.c"
    "${host_os_generated}/host_io_bool.c"
    "${host_os_generated}/host_io_links.c"
    "${host_os_generated}/host_io_temp.c"
    "${host_os_generated}/host_io_directory.c"
    "${host_os_generated}/host_thread_tokens.c"
    "${host_os_generated}/host_signal_frontier.c"
    "${host_os_generated}/host_io_file_real.c"
    "${host_os_generated}/host_original_os_1160.c"
    "${host_os_generated}/host_original_os_1161.c"
    "${host_os_generated}/host_original_io_1190.c"
    "${host_os_generated}/host_original_io_1191.c"
    "${host_os_generated}/host_original_io_1192.c"
    "${host_os_generated}/host_original_io_1193_invalid_utf8.c"
    "${host_os_generated}/host_original_io_1193_read_dir_recursive.c"
    "${host_os_generated}/host_original_io_1194.c"
    "${host_os_generated}/host_original_io_1195.c"
    "${host_os_generated}/host_original_io_1196.c"
    "${host_os_generated}/host_original_io_1197.c"
    "${host_os_generated}/host_original_io_1198.c"
    "${host_os_generated}/host_original_path_1170.c")

add_executable(test_host_os_source "${host_os_dir}/test_host_os_source.c")
target_link_libraries(test_host_os_source PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen xray_source_compile xray_xir_runtime_host)
target_compile_definitions(test_host_os_source PRIVATE XR_HOST_OS_GENERATED="${host_os_generated}")
# Original full-file imports invalidate their emitted programs together.
file(GLOB_RECURSE host_original_stdlib_inputs CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/stdlib/*.xr" "${PROJECT_SOURCE_DIR}/stdlib/*.toml")
add_custom_command(OUTPUT ${host_os_c}
    COMMAND $<TARGET_FILE:test_host_os_source> --emit "${host_os_generated}"
    DEPENDS test_host_os_source
        "${host_os_dir}/host_os_source_cases.h"
        "${host_os_dir}/host_os_runtime.h"
        "${host_os_dir}/host_os_original_suite_runtime.h"
        "${host_os_dir}/host_os_original_suite_source.h"
        ${host_original_stdlib_inputs}
        "${PROJECT_SOURCE_DIR}/tests/regression/10_stdlib/1170_path_basic.xr"
        "${PROJECT_SOURCE_DIR}/tests/regression/10_stdlib/1160_os_basic.xr"
        "${PROJECT_SOURCE_DIR}/tests/regression/10_stdlib/1161_os_extended.xr"
        "${PROJECT_SOURCE_DIR}/tests/regression/10_stdlib/1190_io_basic.xr"
        "${PROJECT_SOURCE_DIR}/tests/regression/10_stdlib/1191_io_edge.xr"
        "${PROJECT_SOURCE_DIR}/tests/regression/10_stdlib/1192_io_extended.xr"
        "${PROJECT_SOURCE_DIR}/tests/regression/10_stdlib/1193_io_invalid_utf8.xr"
        "${PROJECT_SOURCE_DIR}/tests/regression/10_stdlib/1193_io_read_dir_recursive.xr"
        "${PROJECT_SOURCE_DIR}/tests/regression/10_stdlib/1194_io_mkdirp.xr"
        "${PROJECT_SOURCE_DIR}/tests/regression/10_stdlib/1195_io_remove_all.xr"
        "${PROJECT_SOURCE_DIR}/tests/regression/10_stdlib/1196_io_touch.xr"
        "${PROJECT_SOURCE_DIR}/tests/regression/10_stdlib/1197_io_temp.xr"
        "${PROJECT_SOURCE_DIR}/tests/regression/10_stdlib/1198_io_invalid_utf8.xr"
        "${host_os_dir}/host_os_file_kernel_cases.h"
        "${host_os_dir}/host_os_io_file_real_cases.h"
        "${host_os_dir}/host_os_io_file_real_source.h"
        "${host_os_dir}/host_os_io_file_ctor_contracts.h"
        "${host_os_dir}/host_os_io_file_write_contracts.h"
        "${host_os_dir}/host_os_io_file_contracts.h"
        "${PROJECT_SOURCE_DIR}/stdlib/io/io.xr"
        "${PROJECT_SOURCE_DIR}/stdlib/mem/mem.xr"
        "${PROJECT_SOURCE_DIR}/stdlib/os/os.xr"
        "${PROJECT_SOURCE_DIR}/stdlib/time/time.xr"
        "${host_os_dir}/host_os_path_cases.h"
        "${host_os_dir}/host_os_cpu_cases.h"
        "${host_os_dir}/host_os_system_cases.h"
        "${host_os_dir}/host_os_stat_cases.h"
        "${host_os_dir}/host_os_io_bool_cases.h"
        "${host_os_dir}/host_os_io_links_cases.h"
        "${host_os_dir}/host_os_io_temp_cases.h"
        "${host_os_dir}/host_os_io_directory_cases.h"
        "${host_os_dir}/host_os_thread_token_cases.h"
        "${host_os_dir}/host_os_thread_token_contracts.h"
        "${PROJECT_SOURCE_DIR}/stdlib/sys/sys.xr"
        "${PROJECT_SOURCE_DIR}/stdlib/path/path.xr"
        "${PROJECT_SOURCE_DIR}/tests/unit/xir/xir_genuine_sys_path.h"
        "${host_os_dir}/host_os_signal_frontier_cases.h"
    VERBATIM)
add_executable(test_host_os_native "${host_os_dir}/test_host_os_native.c" ${host_os_c})
target_link_libraries(test_host_os_native PRIVATE xray_xir_scalar xray_xir_runtime_host)
target_compile_definitions(test_host_os_native PRIVATE XR_HOST_OS_GENERATED="${host_os_generated}")
foreach(target test_host_os_source test_host_os_native)
    target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_compile_definitions(${target} PRIVATE XR_HOST_OS_ROOT="${PROJECT_SOURCE_DIR}")
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
        if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
            target_compile_options(${target} PRIVATE /experimental:c11atomics)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
    if(NOT WIN32)
        target_compile_definitions(${target} PRIVATE _POSIX_C_SOURCE=200809L)
    endif()
endforeach()
add_test(NAME host_os_source COMMAND test_host_os_source)
add_test(NAME host_os_native COMMAND test_host_os_native)
set_tests_properties(host_os_source host_os_native PROPERTIES
    RUN_SERIAL TRUE TIMEOUT 180 LABELS "unit;xir;source;native;host;ownership")
