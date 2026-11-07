# Can be included by the main test tree or a small standalone Ninja project.
if(NOT WIN32)
    return()
endif()
if(NOT DEFINED XIR_TARGET_SOURCE_ROOT)
    set(XIR_TARGET_SOURCE_ROOT "${PROJECT_SOURCE_DIR}")
endif()
if(NOT DEFINED XIR_RESOURCES_SOURCE_ROOT)
    set(XIR_RESOURCES_SOURCE_ROOT "${XIR_TARGET_SOURCE_ROOT}")
endif()

set(XIR_TARGET_TEST_TARGETS)
if(TARGET xray_xir_target)

else()
    add_library(xir_target_snapshot_production OBJECT
        "${XIR_TARGET_SOURCE_ROOT}/src/app/toolchain/xtc_xir_target.c"
        "${XIR_TARGET_SOURCE_ROOT}/src/app/toolchain/xtc_xir_sysroot.c"
        "${XIR_TARGET_SOURCE_ROOT}/src/app/toolchain/xtc_xir_images.c")
    list(APPEND XIR_TARGET_TEST_TARGETS xir_target_snapshot_production)

endif()

foreach(target IN LISTS XIR_TARGET_TEST_TARGETS)
    target_include_directories(${target} PRIVATE
        "${XIR_RESOURCES_SOURCE_ROOT}/src" "${XIR_TARGET_SOURCE_ROOT}/src")
    target_compile_features(${target} PRIVATE c_std_11)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
        target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
        if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
            target_compile_options(${target} PRIVATE /experimental:c11atomics)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()

if(NOT TARGET Python3::Interpreter)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
endif()

if(TARGET xray_xir_target)
    set(XIR_TARGET_CURRENT_ARCHIVE xray_xir_target)
else()
    add_library(xir_target_retired_link STATIC $<TARGET_OBJECTS:xir_target_snapshot_production>)
    set(XIR_TARGET_CURRENT_ARCHIVE xir_target_retired_link)
endif()
add_test(NAME test_xir_target_retired_objects COMMAND "${Python3_EXECUTABLE}" -X utf8
    "${CMAKE_CURRENT_LIST_DIR}/target_environment_owner/retired_object.py"
    "${CMAKE_C_COMPILER}" $<TARGET_FILE:${XIR_TARGET_CURRENT_ARCHIVE}> "${XIR_TARGET_SOURCE_ROOT}/src")
