# Standalone focused gates; XIR_SDK_BUNDLE_DIR is an existing same-source bundle.
if(NOT WIN32)
    return()
endif()
if(NOT DEFINED XIR_TARGET_SOURCE_ROOT)
    set(XIR_TARGET_SOURCE_ROOT "${PROJECT_SOURCE_DIR}")
endif()
if(NOT DEFINED XIR_SDK_BUNDLE_DIR)
    set(XIR_SDK_BUNDLE_DIR "${CMAKE_BINARY_DIR}/xir-runtime-sdk")
endif()
add_library(xir_sdk_resources_production STATIC
    "${XIR_TARGET_SOURCE_ROOT}/src/toolchain/xr_xir_runtime_sdk.c"
    "${XIR_TARGET_SOURCE_ROOT}/src/base/xjson_cursor.c"
    "${XIR_TARGET_SOURCE_ROOT}/src/base/xcompile_resources.c"
    "${XIR_TARGET_SOURCE_ROOT}/src/base/xsha256.c")
add_executable(test_xir_sdk_resources "${CMAKE_CURRENT_LIST_DIR}/test_xir_runtime_sdk.c"
    "${XIR_TARGET_SOURCE_ROOT}/src/base/xjson_cursor.c"
    "${XIR_TARGET_SOURCE_ROOT}/src/base/xsha256.c" "${XIR_TARGET_SOURCE_ROOT}/src/base/xutf8.c")
add_executable(test_xir_sdk_resources_lease "${CMAKE_CURRENT_LIST_DIR}/test_xir_sdk_lease.c")
target_link_libraries(test_xir_sdk_resources_lease PRIVATE xir_sdk_resources_production)
foreach(target xir_sdk_resources_production test_xir_sdk_resources test_xir_sdk_resources_lease)
    target_include_directories(${target} PRIVATE "${XIR_TARGET_SOURCE_ROOT}/src" "${XIR_SDK_BUNDLE_DIR}")
    target_compile_features(${target} PRIVATE c_std_11)
    target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_xir_sdk_resources COMMAND test_xir_sdk_resources
    "${XIR_SDK_BUNDLE_DIR}" "${XIR_SDK_BUNDLE_DIR}/sdk_manifest.json")
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME test_xir_sdk_resources_manifest COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/sdk_manifest_vectors.py" --executable $<TARGET_FILE:test_xir_sdk_resources>
    --bundle "${XIR_SDK_BUNDLE_DIR}")
set_tests_properties(test_xir_sdk_resources test_xir_sdk_resources_manifest PROPERTIES
    LABELS "unit;xir;ownership;sdk;resources" RUN_SERIAL TRUE TIMEOUT 300)
