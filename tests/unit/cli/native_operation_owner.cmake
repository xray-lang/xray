if(NOT WIN32)
    return()
endif()
foreach(target test_native_operation_owner test_native_operation_faults)
    add_executable(${target}
        "${CMAKE_CURRENT_LIST_DIR}/native_operation_owner/test_native_operation_owner.c")
    target_include_directories(${target} PRIVATE
        "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/include")
    target_compile_definitions(${target} PRIVATE
        NDEBUG WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    set_target_properties(${target} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(${target} PRIVATE /experimental:c11atomics)
    endif()
endforeach()
target_link_libraries(test_native_operation_owner PRIVATE xray_xir_native_operation bcrypt)
target_compile_definitions(test_native_operation_faults PRIVATE OPERATION_INJECTED)
target_link_libraries(test_native_operation_faults PRIVATE xray_xir_invocation xray_xir_workspace bcrypt)
find_program(XIR_OPERATION_MSVC NAMES cl REQUIRED)
find_program(XIR_OPERATION_LINKER NAMES link REQUIRED)
foreach(mode unit prearm native)
    if(mode STREQUAL "native")
        set(binary test_native_operation_owner)
    else()
        set(binary test_native_operation_faults)
    endif()
    add_test(NAME native_operation_${mode} COMMAND ${XRAY_PYTHON} -X utf8
        "${CMAKE_CURRENT_LIST_DIR}/native_operation_owner/test_native_operation_owner.py"
        $<TARGET_FILE:${binary}> "${PROJECT_SOURCE_DIR}"
        "${XIR_OPERATION_MSVC}" "${XIR_OPERATION_LINKER}"
        "${CMAKE_BINARY_DIR}/xir-runtime-sdk" ${mode})
    set_tests_properties(native_operation_${mode} PROPERTIES
        RUN_SERIAL TRUE TIMEOUT 300 COST 3
        LABELS "unit;xir;toolchain;ownership;budget;generated-c")
endforeach()
