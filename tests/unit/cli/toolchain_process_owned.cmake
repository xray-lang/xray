# Process-owner coverage shares the production process library.

if(NOT WIN32)
    return()
endif()
get_filename_component(PROCESS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
include("${PROCESS_ROOT}/src/app/toolchain/xir_process.cmake")
add_executable(test_toolchain_process_owned
    "${CMAKE_CURRENT_LIST_DIR}/test_toolchain_process_owned.c"
    "${PROCESS_ROOT}/src/base/xutf8.c"
    "${PROCESS_ROOT}/src/os/win/proc_self_win.c"
    "${PROCESS_ROOT}/src/os/win/time_win.c")
add_dependencies(test_toolchain_process_owned xray_toolchain_process)
target_link_libraries(test_toolchain_process_owned PRIVATE userenv)
foreach(t test_toolchain_process_owned)
    target_include_directories(${t} PRIVATE "${PROCESS_ROOT}/src")
    target_compile_features(${t} PRIVATE c_std_11)
    target_compile_definitions(${t} PRIVATE NDEBUG _CRT_SECURE_NO_WARNINGS)
    if(MSVC)
        target_compile_options(${t} PRIVATE /W4 /WX /utf-8)
        if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
            target_compile_options(${t} PRIVATE /experimental:c11atomics)
        endif()
    endif()
endforeach()
add_test(NAME test_toolchain_process_owned COMMAND test_toolchain_process_owned)
set_tests_properties(test_toolchain_process_owned PROPERTIES LABELS "unit;toolchain;ownership;budget" TIMEOUT 120)
add_test(NAME test_toolchain_process_hidden_environment COMMAND test_toolchain_process_owned --hidden-env-parent)
set_tests_properties(test_toolchain_process_hidden_environment PROPERTIES LABELS "unit;toolchain;ownership;environment" TIMEOUT 30)
