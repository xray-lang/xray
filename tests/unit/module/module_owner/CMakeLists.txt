cmake_minimum_required(VERSION 3.21)
project(xray_module_owner_tests LANGUAGES C)
if(NOT CMAKE_GENERATOR STREQUAL "Ninja")
    message(FATAL_ERROR "Module owner tests require Ninja")
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
get_filename_component(XR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
if(NOT TARGET xray_compile_resources)
    add_subdirectory("${XR_ROOT}/src/base" compiler-resources EXCLUDE_FROM_ALL)
endif()
include("${XR_ROOT}/src/toolchain/xcompiler_producer.cmake")
if(NOT TARGET compile_owner_core)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../xir/compile_owner" compile-core EXCLUDE_FROM_ALL)
endif()
set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated/stdlib_embedded_sources.inc")
file(GLOB_RECURSE generator_inputs CONFIGURE_DEPENDS "${XR_ROOT}/tools/stdlibgen/*.py" "${XR_ROOT}/stdlib/defs/*.def")
add_custom_command(OUTPUT "${generated}"
    COMMAND ${Python3_EXECUTABLE} -X utf8 "${XR_ROOT}/scripts/generate_stdlib_embedded.py"
        --root "${XR_ROOT}" --source-output "${generated}" "${XR_ROOT}/stdlib/io/io.xr" "${XR_ROOT}/stdlib/path/path.xr"
    DEPENDS "${XR_ROOT}/scripts/generate_stdlib_embedded.py" ${generator_inputs}
        "${XR_ROOT}/stdlib/stdlib_boundary.toml" "${XR_ROOT}/stdlib/io/io.xr" "${XR_ROOT}/stdlib/path/path.xr" VERBATIM)
add_executable(test_module_owner test_module_owner.c "${generated}"
    "${XR_ROOT}/src/module/xmodule_graph.c" "${XR_ROOT}/src/module/xmodule_resolver.c"
    "${XR_ROOT}/src/module/xmodule_identity.c" "${XR_ROOT}/src/module/xmodule_identity_view.c"
    "${XR_ROOT}/src/module/xmodule_fingerprint.c" "${XR_ROOT}/src/module/stdlib_embedded.c"
    "${XR_ROOT}/src/module/xlockfile.c" "${XR_ROOT}/src/module/xsemver.c"
    "${XR_ROOT}/src/xir/xxir_library_catalog.c" "${XR_ROOT}/src/base/xhashmap.c"
    "${XR_ROOT}/src/base/xio_policy.c" "${XR_ROOT}/src/base/xfileio.c"
    "${XR_ROOT}/src/os/win/fs_win.c" "${XR_ROOT}/src/os/win/dir_win.c"
    "${XR_ROOT}/src/os/win/file_read_win.c" "${XR_ROOT}/src/os/win/proc_win.c")
target_link_libraries(test_module_owner PRIVATE xray_compiler_parser compile_owner_core bcrypt)
target_include_directories(test_module_owner PRIVATE "${XR_ROOT}/src" "${XR_ROOT}/include" "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(test_module_owner PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(test_module_owner PRIVATE /utf-8 /W4 /WX)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_module_owner PRIVATE /experimental:c11atomics)
    endif()
endif()
enable_testing()
add_test(NAME test_module_owner COMMAND ${Python3_EXECUTABLE} -X utf8
    "${CMAKE_CURRENT_LIST_DIR}/test_module_owner.py" $<TARGET_FILE:test_module_owner>)
set_tests_properties(test_module_owner PROPERTIES LABELS "unit;module;ownership;budget" TIMEOUT 240)
