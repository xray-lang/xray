set(host_os_dir "${CMAKE_CURRENT_LIST_DIR}")
set(host_os_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/host-os")
file(MAKE_DIRECTORY "${host_os_generated}")
set(host_os_c
    "${host_os_generated}/host_count.c"
    "${host_os_generated}/host_pin.c"
    "${host_os_generated}/host_path.c")

add_executable(test_host_os_source "${host_os_dir}/test_host_os_source.c")
target_link_libraries(test_host_os_source PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_host_os_source PRIVATE XR_HOST_OS_GENERATED="${host_os_generated}")
add_custom_command(OUTPUT ${host_os_c}
    COMMAND $<TARGET_FILE:test_host_os_source> --emit "${host_os_generated}"
    DEPENDS test_host_os_source
        "${host_os_dir}/host_os_source_cases.h"
        "${host_os_dir}/host_os_runtime.h"
        "${host_os_dir}/host_os_path_cases.h"
        "${host_os_dir}/host_os_cpu_cases.h"
    VERBATIM)
add_executable(test_host_os_native "${host_os_dir}/test_host_os_native.c" ${host_os_c})
target_link_libraries(test_host_os_native PRIVATE xray_xir_scalar)
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
