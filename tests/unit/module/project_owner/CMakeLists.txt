cmake_minimum_required(VERSION 3.21)
project(xray_project_owner_tests LANGUAGES C)
if(NOT CMAKE_GENERATOR STREQUAL "Ninja")
  message(FATAL_ERROR "Project owner tests require Ninja")
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
get_filename_component(XR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
if(NOT TARGET xray_compile_resources)
  add_subdirectory("${XR_ROOT}/src/base" compiler-resources EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET xray_cli_source AND NOT TARGET compile_owner_core)
 add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../xir/compile_owner" compile-core EXCLUDE_FROM_ALL)
endif()
add_executable(test_project_owner test_project_owner.c)
if(TARGET xray_cli_source)
 target_link_libraries(test_project_owner PRIVATE xray_cli_source bcrypt)
else()
target_sources(test_project_owner PRIVATE
 "${XR_ROOT}/src/app/cli/xcli_graph_authority.c"
 "${XR_ROOT}/src/module/xmodule_identity.c" "${XR_ROOT}/src/xir/xxir_library_catalog.c"
 "${XR_ROOT}/src/module/xproject.c" "${XR_ROOT}/src/module/xnative_package.c"
 "${XR_ROOT}/src/module/xmodule_identity_view.c" "${XR_ROOT}/src/module/xsemver.c"
 "${XR_ROOT}/src/module/xlockfile.c" "${XR_ROOT}/src/base/xtoml.c"
 "${XR_ROOT}/src/base/xhashmap.c" "${XR_ROOT}/src/base/xfileio.c"
 "${XR_ROOT}/src/base/xio_policy.c" "${XR_ROOT}/src/base/xsha256.c"
 "${XR_ROOT}/src/os/win/fs_win.c" "${XR_ROOT}/src/os/win/dir_win.c"
 "${XR_ROOT}/src/os/win/file_read_win.c" "${XR_ROOT}/src/os/win/proc_self_win.c")
target_link_libraries(test_project_owner PRIVATE xray_compile_resources compile_owner_core bcrypt)
endif()
target_include_directories(test_project_owner PRIVATE "${XR_ROOT}/src" "${XR_ROOT}/include")
target_compile_definitions(test_project_owner PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
if(MSVC)
 target_compile_options(test_project_owner PRIVATE /utf-8 /W4 /WX)
 if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
  target_compile_options(test_project_owner PRIVATE /experimental:c11atomics)
 endif()
endif()
enable_testing()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME test_project_owner COMMAND ${Python3_EXECUTABLE} -X utf8
 "${CMAKE_CURRENT_LIST_DIR}/test_project_owner.py" $<TARGET_FILE:test_project_owner>)
set_tests_properties(test_project_owner PROPERTIES TIMEOUT 900 COST 180 LABELS "unit;module;ownership;budget")
