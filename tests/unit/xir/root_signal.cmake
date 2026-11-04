set(root_signal_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/root-signal")
file(MAKE_DIRECTORY "${root_signal_generated}")
add_executable(test_xir_signal_source xir/test_xir_signal_source.c)
target_link_libraries(test_xir_signal_source PRIVATE
    xray_xir_source xray_xir_vm xray_xir_cgen xray_xir_ctfe xray_xir_runtime_host)
target_compile_definitions(test_xir_signal_source PRIVATE
    XR_ROOT_SIGNAL_STDLIB="${PROJECT_SOURCE_DIR}/stdlib"
    XR_ROOT_SIGNAL_GENERATED="${root_signal_generated}")
add_custom_command(OUTPUT "${root_signal_generated}/root_signal.c"
    COMMAND $<TARGET_FILE:test_xir_signal_source> --emit "${root_signal_generated}"
    DEPENDS test_xir_signal_source "${CMAKE_CURRENT_LIST_DIR}/root_signal_runtime.h"
        "${PROJECT_SOURCE_DIR}/stdlib/path/path.xr"
        "${PROJECT_SOURCE_DIR}/tests/unit/xir/xir_genuine_sys_path.h"
    VERBATIM)
add_executable(test_xir_signal_native xir/test_xir_signal_native.c
    "${root_signal_generated}/root_signal.c")
target_link_libraries(test_xir_signal_native PRIVATE
    xray_xir_source xray_xir_vm xray_xir_runtime_host)
foreach(target test_xir_signal_source test_xir_signal_native)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
        if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
            target_compile_options(${target} PRIVATE /experimental:c11atomics)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -pedantic)
    endif()
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES RUN_SERIAL TRUE TIMEOUT 120
        LABELS "unit;xir;signal;ownership;runtime")
endforeach()
