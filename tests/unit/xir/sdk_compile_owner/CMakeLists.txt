cmake_minimum_required(VERSION 3.25)
project(XirSdkCompileOwner C)
if(NOT CMAKE_GENERATOR STREQUAL "Ninja" OR NOT WIN32)
    message(FATAL_ERROR "This SDK qualification requires Windows and Ninja")
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
get_filename_component(SDK_OWNER_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
if(NOT EXISTS "${XIR_SDK_BUNDLE_DIR}/sdk_manifest.json" OR NOT EXISTS "${XIR_OLD_ABI_DIR}/baseline.json")
    message(FATAL_ERROR "The real current SDK and preserved old objects are required")
endif()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${SDK_OWNER_ROOT}/scripts/gen_xir_runtime_sdk_recipe.py"
    --root "${SDK_OWNER_ROOT}" --output "${CMAKE_CURRENT_BINARY_DIR}/sdk_recipe.inc.c"
    COMMAND_ERROR_IS_FATAL ANY)
enable_testing()
set(SDK_OWNER_LIBRARIES)
foreach(owner xray_compile_resources xray_xir_admission xray_xir_declarations xray_xir_scalar xray_xir_runtime_host)
    add_library(sdk_${owner} STATIC IMPORTED)
    set_target_properties(sdk_${owner} PROPERTIES IMPORTED_LOCATION "${XIR_SDK_BUNDLE_DIR}/lib/${owner}.lib")
    list(APPEND SDK_OWNER_LIBRARIES sdk_${owner})
endforeach()
add_executable(sdk_owner_generator
    "${CMAKE_CURRENT_LIST_DIR}/../test_xir_compile_emit_owner.c"
    "${SDK_OWNER_ROOT}/src/xir/xxir_specialize.c"
    "${SDK_OWNER_ROOT}/src/xir/xxir_emit_c.c"
    "${SDK_OWNER_ROOT}/src/aot/xi_cgen_verify_output.c")
target_link_libraries(sdk_owner_generator PRIVATE ${SDK_OWNER_LIBRARIES} kernel32)
set(SDK_OWNER_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/sdk-owner-generated.c")
add_custom_command(OUTPUT "${SDK_OWNER_GENERATED}"
    COMMAND $<TARGET_FILE:sdk_owner_generator> "${SDK_OWNER_GENERATED}"
    DEPENDS sdk_owner_generator VERBATIM)
add_executable(test_sdk_compile_owner "${CMAKE_CURRENT_LIST_DIR}/test_sdk_compile_owner.c"
    "${SDK_OWNER_GENERATED}" "${SDK_OWNER_ROOT}/src/toolchain/xr_xir_runtime_sdk.c"
    "${SDK_OWNER_ROOT}/src/base/xjson_cursor.c"
    "${XIR_OLD_ABI_DIR}/build/CMakeFiles/old_admission_data.dir/fixtures/old_admission_data.c.obj")
target_link_libraries(test_sdk_compile_owner PRIVATE ${SDK_OWNER_LIBRARIES} kernel32)
add_executable(test_sdk_old_data "${CMAKE_CURRENT_LIST_DIR}/../test_xir_compile_old_data.c"
    "${XIR_OLD_ABI_DIR}/build/CMakeFiles/old_admission_data.dir/fixtures/old_admission_data.c.obj")
target_link_libraries(test_sdk_old_data PRIVATE ${SDK_OWNER_LIBRARIES} kernel32)
set(XIR_TARGET_SOURCE_ROOT "${SDK_OWNER_ROOT}")
include("${CMAKE_CURRENT_LIST_DIR}/../sdk_resources.cmake")
foreach(target sdk_owner_generator test_sdk_compile_owner test_sdk_old_data xir_sdk_resources_production test_xir_sdk_resources test_xir_sdk_resources_lease)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${XIR_SDK_BUNDLE_DIR}" "${XIR_SDK_BUNDLE_DIR}/src" "${SDK_OWNER_ROOT}/src")
    target_compile_definitions(${target} PRIVATE NDEBUG NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(${target} PRIVATE /experimental:c11atomics)
    endif()
endforeach()
add_test(NAME test_sdk_compile_owner COMMAND test_sdk_compile_owner
    "${XIR_SDK_BUNDLE_DIR}" "${XIR_SDK_BUNDLE_DIR}/sdk_manifest.json")
add_test(NAME test_sdk_old_data COMMAND test_sdk_old_data)
add_test(NAME test_sdk_old_objects COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/verify_sdk_compile_owner.py" --bundle "${XIR_SDK_BUNDLE_DIR}"
    --old "${XIR_OLD_ABI_DIR}" --root "${SDK_OWNER_ROOT}" --output "${CMAKE_CURRENT_BINARY_DIR}/old-objects"
    --owner $<TARGET_FILE:test_xir_sdk_resources>)
add_test(NAME test_sdk_identity_known_bytes COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../derive_sdk_identity.py")
set_tests_properties(test_sdk_compile_owner test_sdk_old_data test_sdk_old_objects test_sdk_identity_known_bytes
    PROPERTIES LABELS "unit;xir;sdk;abi;ownership" RUN_SERIAL TRUE TIMEOUT 180)
