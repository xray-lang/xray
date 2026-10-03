cmake_minimum_required(VERSION 3.21)
project(xray_catalog_owner_tests LANGUAGES C)
if(NOT CMAKE_GENERATOR STREQUAL "Ninja")
    message(FATAL_ERROR "Catalog owner tests require Ninja")
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
get_filename_component(XR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
if(NOT TARGET compile_owner_core)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../compile_owner" compile-core EXCLUDE_FROM_ALL)
endif()
add_executable(test_catalog_owner test_catalog_owner.c
    "${XR_ROOT}/src/xir/xxir_library_catalog.c"
    "${XR_ROOT}/src/module/xmodule_identity.c"
    "${XR_ROOT}/src/module/xmodule_identity_view.c"
    "${XR_ROOT}/src/base/xio_policy.c"
    "${XR_ROOT}/src/base/xfileio.c")
target_link_libraries(test_catalog_owner PRIVATE compile_owner_core)
target_compile_definitions(test_catalog_owner PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(test_catalog_owner PRIVATE /utf-8 /W4 /WX)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_catalog_owner PRIVATE /experimental:c11atomics)
    endif()
endif()
enable_testing()
add_test(NAME test_catalog_owner COMMAND test_catalog_owner)
set_tests_properties(test_catalog_owner PROPERTIES LABELS "unit;xir;compiler;ownership;budget" TIMEOUT 120)
