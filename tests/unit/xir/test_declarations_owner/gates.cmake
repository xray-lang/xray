# Test declarations exercise the same production libraries in both builds.
get_filename_component(XR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(XR_TEST_DECLARATIONS_BINARY "${CMAKE_CURRENT_BINARY_DIR}/test-declarations")
file(MAKE_DIRECTORY "${XR_TEST_DECLARATIONS_BINARY}")
add_executable(test_test_source_cases "${CMAKE_CURRENT_LIST_DIR}/test_source_cases.c")
target_link_libraries(test_test_source_cases PRIVATE xray_xir_source_product)
add_test(NAME test_test_source_cases COMMAND "${Python3_EXECUTABLE}" -X utf8
    "${CMAKE_CURRENT_LIST_DIR}/source_cases.py" --driver "$<TARGET_FILE:test_test_source_cases>"
    --stdlib "${XR_ROOT}/stdlib" --output "${XR_TEST_DECLARATIONS_BINARY}/source-cases")

add_test(NAME test_test_packet_oracle COMMAND "${Python3_EXECUTABLE}" -X utf8
    "${CMAKE_CURRENT_LIST_DIR}/../derive_test_roles_vectors.py" --output "${XR_TEST_DECLARATIONS_BINARY}/ordinary-packets")
set_tests_properties(test_test_packet_oracle PROPERTIES FIXTURES_SETUP role_packets)

add_executable(test_method_role_regression "${CMAKE_CURRENT_LIST_DIR}/../test_xir_compile_runtime_owner.c")
target_link_libraries(test_method_role_regression PRIVATE xray_xir_vm)
add_test(NAME test_method_role_regression COMMAND test_method_role_regression)
add_executable(test_test_declarations "${CMAKE_CURRENT_LIST_DIR}/test_test_declarations.c")
target_link_libraries(test_test_declarations PRIVATE xray_xir_source_product)
add_test(NAME test_test_declarations COMMAND test_test_declarations "${CMAKE_CURRENT_LIST_DIR}" "${XR_ROOT}/stdlib")
set(test_generated "${XR_TEST_DECLARATIONS_BINARY}/test-roles.c")
add_custom_command(OUTPUT "${test_generated}"
    COMMAND test_test_declarations "${CMAKE_CURRENT_LIST_DIR}" "${XR_ROOT}/stdlib" "${test_generated}"
    DEPENDS test_test_declarations "${CMAKE_CURRENT_LIST_DIR}/basic.xr" "${CMAKE_CURRENT_LIST_DIR}/library.xr" VERBATIM)
add_executable(test_test_native "${CMAKE_CURRENT_LIST_DIR}/test_native.c" "${test_generated}")
target_link_libraries(test_test_native PRIVATE xray_xir_scalar xray_compile_resources)
add_test(NAME test_test_native COMMAND test_test_native)

set(test_declaration_targets test_test_source_cases test_method_role_regression test_test_declarations test_test_native)
set(test_declaration_tests test_test_source_cases test_test_packet_oracle test_method_role_regression test_test_declarations test_test_native)

foreach(target IN LISTS test_declaration_targets)
    target_include_directories(${target} PRIVATE "${XR_ROOT}/src" "${XR_ROOT}/include")
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
        target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
        if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
            target_compile_options(${target} PRIVATE /experimental:c11atomics)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -pedantic)
    endif()
endforeach()
set_tests_properties(${test_declaration_tests} PROPERTIES
    LABELS "unit;xir;test-declarations;ownership;budget;abi" TIMEOUT 90)
