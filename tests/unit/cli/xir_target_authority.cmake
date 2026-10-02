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
add_executable(test_xir_target_authority
    "${CMAKE_CURRENT_LIST_DIR}/test_xir_target_authority.c"
    "${XIR_TARGET_SOURCE_ROOT}/src/base/xsha256.c"
    "${XIR_TARGET_SOURCE_ROOT}/src/base/xutf8.c")
add_library(xir_target_snapshot_production OBJECT
    "${XIR_TARGET_SOURCE_ROOT}/src/app/toolchain/xtc_xir_target.c"
    "${XIR_TARGET_SOURCE_ROOT}/src/app/toolchain/xtc_xir_sysroot.c")
add_executable(test_xir_target_environment_production
    "${CMAKE_CURRENT_LIST_DIR}/test_xir_target_authority.c"
    "${XIR_TARGET_SOURCE_ROOT}/src/base/xsha256.c"
    "${XIR_TARGET_SOURCE_ROOT}/src/base/xutf8.c"
    $<TARGET_OBJECTS:xir_target_snapshot_production>)
target_compile_definitions(test_xir_target_environment_production PRIVATE XIR_TARGET_PRODUCTION_TEST)
target_compile_features(test_xir_target_environment_production PRIVATE c_std_11)
target_include_directories(test_xir_target_environment_production PRIVATE
    "${XIR_RESOURCES_SOURCE_ROOT}/src" "${XIR_TARGET_SOURCE_ROOT}/src")
target_include_directories(xir_target_snapshot_production PRIVATE
    "${XIR_RESOURCES_SOURCE_ROOT}/src" "${XIR_TARGET_SOURCE_ROOT}/src")
target_compile_features(xir_target_snapshot_production PRIVATE c_std_11)
target_include_directories(test_xir_target_authority PRIVATE
    "${XIR_RESOURCES_SOURCE_ROOT}/src" "${XIR_TARGET_SOURCE_ROOT}/src")
target_compile_features(test_xir_target_authority PRIVATE c_std_11)
if(MSVC)
    target_compile_options(xir_target_snapshot_production PRIVATE /W4 /WX /utf-8)
    target_compile_definitions(xir_target_snapshot_production PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(test_xir_target_environment_production PRIVATE /W4 /WX /utf-8)
    target_compile_definitions(test_xir_target_environment_production PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(test_xir_target_authority PRIVATE /W4 /WX /utf-8)
    target_compile_definitions(test_xir_target_authority PRIVATE _CRT_SECURE_NO_WARNINGS)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_xir_target_authority PRIVATE /experimental:c11atomics)
        target_compile_options(test_xir_target_environment_production PRIVATE /experimental:c11atomics)
    endif()
else()
    target_compile_options(xir_target_snapshot_production PRIVATE -Wall -Wextra -Werror)
    target_compile_options(test_xir_target_authority PRIVATE -Wall -Wextra -Werror)
    target_compile_options(test_xir_target_environment_production PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_target_authority COMMAND test_xir_target_authority)
set_tests_properties(test_xir_target_authority PROPERTIES LABELS "unit;xir;toolchain;ownership" TIMEOUT 120)

add_test(NAME test_xir_target_environment_production COMMAND test_xir_target_environment_production)
set_tests_properties(test_xir_target_environment_production PROPERTIES LABELS "unit;xir;toolchain;ownership" TIMEOUT 120)
