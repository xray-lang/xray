cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(XirNamespaceOwner C)
endif()
if(NOT WIN32 OR NOT CMAKE_GENERATOR STREQUAL "Ninja")
    message(FATAL_ERROR "Namespace gates require Windows Ninja")
endif()
get_filename_component(XR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
option(NAMESPACE_SANITIZE "Enable Clang ASan and UBSan" OFF)
if(NAMESPACE_SANITIZE)
    if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
        message(FATAL_ERROR "Namespace sanitizers require clang-cl")
    endif()
    execute_process(COMMAND "${CMAKE_C_COMPILER}" /clang:-print-resource-dir
        OUTPUT_VARIABLE resource OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
endif()
foreach(mode injected production)
    add_executable(test_xir_namespace_${mode} test_xir_namespace.c
        "${XR_ROOT}/src/base/xutf8.c" "${XR_ROOT}/src/base/xsha256.c")
    target_include_directories(test_xir_namespace_${mode} PRIVATE "${XR_ROOT}/src" "${XR_ROOT}/include")
    if(mode STREQUAL "production")
        target_compile_definitions(test_xir_namespace_${mode} PRIVATE NAMESPACE_PRODUCTION)
        target_sources(test_xir_namespace_${mode} PRIVATE
            "${XR_ROOT}/src/base/xio_policy.c" "${XR_ROOT}/src/base/xfileio.c"
            "${XR_ROOT}/src/os/win/dir_win.c"
            "${XR_ROOT}/src/app/toolchain/xtc_xir_target.c"
            "${XR_ROOT}/src/app/toolchain/xtc_xir_sysroot.c"
            "${XR_ROOT}/src/app/toolchain/xtc_xir_images.c"
            "${XR_ROOT}/src/app/toolchain/xtc_xir_namespace.c")
    endif()
    target_compile_definitions(test_xir_namespace_${mode} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_options(test_xir_namespace_${mode} PRIVATE /W4 /WX /utf-8)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_xir_namespace_${mode} PRIVATE /experimental:c11atomics)
    endif()
    if(NAMESPACE_SANITIZE)
        target_compile_options(test_xir_namespace_${mode} PRIVATE /fsanitize=address /Oy-
            /clang:-fsanitize=undefined /clang:-fno-sanitize=function)
        target_link_options(test_xir_namespace_${mode} PRIVATE /INCREMENTAL:NO /INCLUDE:__asan_seh_interceptor
            "/WHOLEARCHIVE:${resource}/lib/windows/clang_rt.asan_dynamic_runtime_thunk-x86_64.lib")
        target_link_libraries(test_xir_namespace_${mode} PRIVATE "${resource}/lib/windows/clang_rt.asan_dynamic-x86_64.lib")
    endif()
endforeach()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
enable_testing()
foreach(mode injected production)
    add_test(NAME namespace_${mode} COMMAND "${Python3_EXECUTABLE}" -X utf8
        "${CMAKE_CURRENT_LIST_DIR}/test_xir_namespace.py" $<TARGET_FILE:test_xir_namespace_${mode}>)
    set_tests_properties(namespace_${mode} PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE LABELS "unit;xir;ownership;budget")
    if(NAMESPACE_SANITIZE)
        set_tests_properties(namespace_${mode} PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${resource}/lib/windows")
    endif()
endforeach()
