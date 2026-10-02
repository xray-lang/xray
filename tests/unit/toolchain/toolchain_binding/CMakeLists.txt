cmake_minimum_required(VERSION 3.21)
project(xray_toolchain_binding_tests LANGUAGES C)
if(NOT CMAKE_GENERATOR STREQUAL "Ninja")
    message(FATAL_ERROR "Toolchain binding tests require Ninja")
endif()
get_filename_component(OWNER_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(OWNER_SOURCES "${OWNER_ROOT}/src/toolchain/xr_toolchain_binding.c"
    "${OWNER_ROOT}/src/aot/program/xr_native_artifact.c"
    "${OWNER_ROOT}/src/base/xsha256.c")
add_library(toolchain_owner_production STATIC ${OWNER_SOURCES}
    "${OWNER_ROOT}/src/base/xcompile_resources.c")
add_executable(test_toolchain_binding test_toolchain_binding.c ${OWNER_SOURCES})
foreach(target toolchain_owner_production test_toolchain_binding)
    target_include_directories(${target} PRIVATE "${OWNER_ROOT}/src")
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
        if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
            target_compile_options(${target} PRIVATE /experimental:c11atomics)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -pedantic)
    endif()
endforeach()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
enable_testing()
add_test(NAME toolchain_owner COMMAND test_toolchain_binding)
add_test(NAME toolchain_frame_oracle COMMAND "${Python3_EXECUTABLE}" -X utf8
    "${CMAKE_CURRENT_LIST_DIR}/frame_oracle.py" "$<TARGET_FILE:test_toolchain_binding>")
set_tests_properties(toolchain_owner toolchain_frame_oracle PROPERTIES TIMEOUT 45)
if(MSVC)
    add_test(NAME toolchain_retired_abi COMMAND "${Python3_EXECUTABLE}" -X utf8
        "${CMAKE_CURRENT_LIST_DIR}/retired_abi.py"
        --compiler "${CMAKE_C_COMPILER}" --archive "$<TARGET_FILE:toolchain_owner_production>"
        --root "${OWNER_ROOT}" --work "${CMAKE_CURRENT_BINARY_DIR}/retired-abi"
        --runtime "${CMAKE_MSVC_RUNTIME_LIBRARY}" --link-flags=${CMAKE_EXE_LINKER_FLAGS})
    set_tests_properties(toolchain_retired_abi PROPERTIES TIMEOUT 45)
endif()
