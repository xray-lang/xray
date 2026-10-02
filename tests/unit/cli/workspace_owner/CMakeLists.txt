cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(XirWorkspaceOwner C)
endif()
if(NOT WIN32 OR NOT CMAKE_GENERATOR STREQUAL "Ninja")
    message(FATAL_ERROR "Workspace gates require Windows Ninja")
endif()
get_filename_component(XR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
option(WORKSPACE_SANITIZE "Enable Clang ASan and UBSan" OFF)
if(WORKSPACE_SANITIZE)
    execute_process(COMMAND "${CMAKE_C_COMPILER}" /clang:-print-resource-dir
        OUTPUT_VARIABLE resource OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
endif()
foreach(mode injected production)
    add_executable(test_xir_workspace_${mode} test_xir_workspace.c
        "${XR_ROOT}/src/base/xutf8.c" "${XR_ROOT}/src/base/xsha256.c"
        "${XR_ROOT}/src/base/xio_policy.c" "${XR_ROOT}/src/base/xfileio.c"
        "${XR_ROOT}/src/os/win/dir_win.c"
        "${XR_ROOT}/src/app/toolchain/xtc_xir_target.c"
        "${XR_ROOT}/src/app/toolchain/xtc_xir_sysroot.c"
        "${XR_ROOT}/src/app/toolchain/xtc_xir_images.c"
        "${XR_ROOT}/src/app/toolchain/xtc_xir_namespace.c")
    if(mode STREQUAL "production")
        target_compile_definitions(test_xir_workspace_${mode} PRIVATE WORKSPACE_PRODUCTION)
        target_sources(test_xir_workspace_${mode} PRIVATE "${XR_ROOT}/src/app/toolchain/xtc_xir_workspace.c")
    endif()
    target_include_directories(test_xir_workspace_${mode} PRIVATE "${XR_ROOT}/src" "${XR_ROOT}/include")
    target_compile_definitions(test_xir_workspace_${mode} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_options(test_xir_workspace_${mode} PRIVATE /W4 /WX /utf-8)
    target_link_libraries(test_xir_workspace_${mode} PRIVATE ntdll bcrypt)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_xir_workspace_${mode} PRIVATE /experimental:c11atomics)
    endif()
    if(WORKSPACE_SANITIZE)
        target_compile_options(test_xir_workspace_${mode} PRIVATE /fsanitize=address /Oy-
            /clang:-fsanitize=undefined /clang:-fno-sanitize=function)
        target_link_options(test_xir_workspace_${mode} PRIVATE /INCREMENTAL:NO /INCLUDE:__asan_seh_interceptor
            "/WHOLEARCHIVE:${resource}/lib/windows/clang_rt.asan_dynamic_runtime_thunk-x86_64.lib")
        target_link_libraries(test_xir_workspace_${mode} PRIVATE "${resource}/lib/windows/clang_rt.asan_dynamic-x86_64.lib")
    endif()
endforeach()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
find_program(WORKSPACE_CL NAMES cl REQUIRED)
enable_testing()
foreach(mode injected production)
    add_test(NAME workspace_${mode} COMMAND "${Python3_EXECUTABLE}" -X utf8
        "${CMAKE_CURRENT_LIST_DIR}/test_xir_workspace.py" $<TARGET_FILE:test_xir_workspace_${mode}> "${WORKSPACE_CL}")
    set_tests_properties(workspace_${mode} PROPERTIES TIMEOUT 240 RUN_SERIAL TRUE LABELS "unit;xir;ownership;budget")
    if(WORKSPACE_SANITIZE)
        set_tests_properties(workspace_${mode} PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${resource}/lib/windows")
    endif()
endforeach()
