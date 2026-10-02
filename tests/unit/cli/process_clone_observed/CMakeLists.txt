cmake_minimum_required(VERSION 3.21)
project(ProcessCloneObserved C)
if(NOT CMAKE_GENERATOR STREQUAL "Ninja" OR NOT WIN32)
    message(FATAL_ERROR "This independent gate requires Windows Ninja")
endif()
get_filename_component(CLONE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(CLONE_COMMON
    "${CLONE_ROOT}/src/base/xutf8.c"
    "${CLONE_ROOT}/src/os/win/proc_win.c"
    "${CLONE_ROOT}/src/os/win/proc_self_win.c"
    "${CLONE_ROOT}/src/os/win/pipe_win.c"
    "${CLONE_ROOT}/src/os/win/time_win.c")
add_executable(test_process_clone_observed test_process_clone_observed.c ${CLONE_COMMON})
target_compile_definitions(test_process_clone_observed PRIVATE CLONE_INJECTED=1)
add_executable(test_process_clone_production test_process_clone_observed.c ${CLONE_COMMON}
    "${CLONE_ROOT}/src/base/xcompile_resources.c"
    "${CLONE_ROOT}/src/app/toolchain/xtc_process.c")
foreach(t test_process_clone_observed test_process_clone_production)
    set_target_properties(${t} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_include_directories(${t} PRIVATE "${CLONE_ROOT}/src")
    target_compile_definitions(${t} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_options(${t} PRIVATE /W4 /WX /utf-8)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(${t} PRIVATE /experimental:c11atomics)
    endif()
endforeach()
enable_testing()
add_test(NAME process_clone_observed COMMAND test_process_clone_observed)
add_test(NAME process_clone_production COMMAND test_process_clone_production)
set_tests_properties(process_clone_observed process_clone_production PROPERTIES TIMEOUT 90)
