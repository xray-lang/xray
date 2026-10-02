if(NOT WIN32)
    return()
endif()
foreach(target test_native_invocation_owner test_native_invocation_faults)
    add_executable(${target}
        "${CMAKE_CURRENT_LIST_DIR}/native_invocation_owner/test_native_invocation_owner.c")
    target_link_libraries(${target} PRIVATE xray_xir_invocation bcrypt)
    target_include_directories(${target} PRIVATE
        "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/include")
    target_compile_definitions(${target} PRIVATE
        NDEBUG WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    set_target_properties(${target} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -pedantic)
    endif()
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(${target} PRIVATE /experimental:c11atomics)
    endif()
endforeach()
target_compile_definitions(test_native_invocation_faults PRIVATE INVOCATION_INJECTED)
find_program(XIR_INVOCATION_MSVC NAMES cl REQUIRED)
find_program(XIR_INVOCATION_LINKER NAMES link REQUIRED)
foreach(mode owner faults guard)
    set(extra_args)
    set(binary test_native_invocation_owner)
    if(NOT mode STREQUAL "owner")
        set(binary test_native_invocation_faults)
        list(APPEND extra_args ${mode})
    endif()
    add_test(NAME native_invocation_${mode} COMMAND ${XRAY_PYTHON} -X utf8
        "${CMAKE_CURRENT_LIST_DIR}/native_invocation_owner/test_native_invocation_owner.py"
        $<TARGET_FILE:${binary}> "${PROJECT_SOURCE_DIR}"
        "${XIR_INVOCATION_MSVC}" "${XIR_INVOCATION_LINKER}"
        "${CMAKE_BINARY_DIR}/xir-runtime-sdk" ${extra_args})
    set_tests_properties(native_invocation_${mode} PROPERTIES
        RUN_SERIAL TRUE LABELS "unit;xir;toolchain;ownership;budget;generated-c")
endforeach()
set_tests_properties(native_invocation_owner PROPERTIES TIMEOUT 600 COST 10)
set_tests_properties(native_invocation_faults PROPERTIES TIMEOUT 1200 COST 45)
set_tests_properties(native_invocation_guard PROPERTIES TIMEOUT 600 COST 20)
if(ENABLE_ASAN OR ENABLE_SANITIZERS)
    set_tests_properties(native_invocation_owner PROPERTIES COST 32)
    set_tests_properties(native_invocation_faults PROPERTIES COST 220)
    set_tests_properties(native_invocation_guard PROPERTIES COST 100)
endif()
