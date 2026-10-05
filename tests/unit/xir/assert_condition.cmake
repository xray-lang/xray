# The declaration source and default owner are consumed by VM and real C.
set(XIR_ASSERT_CONDITION_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/assert-condition-source)
set(XIR_ASSERT_CONDITION_C ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_assert_condition.c)
set(XIR_ASSERT_CONDITION_TEST_C ${CMAKE_CURRENT_BINARY_DIR}/assert-condition-test.c)
file(MAKE_DIRECTORY ${XIR_ASSERT_CONDITION_DIR})
add_executable(test_xir_assert_condition ${CMAKE_CURRENT_SOURCE_DIR}/xir/test_xir_assert_condition.c)
target_link_libraries(test_xir_assert_condition PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
add_dependencies(test_xir_assert_condition gen-xir-core-source)
add_custom_command(OUTPUT ${XIR_ASSERT_CONDITION_C}
    COMMAND $<TARGET_FILE:test_xir_assert_condition> ${XIR_ASSERT_CONDITION_DIR} ${XIR_ASSERT_CONDITION_C}
    DEPENDS test_xir_assert_condition
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_assert_condition_cases.h
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_assert_condition_golden.h
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_assert_condition_source_gates.inc.c
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_assert_condition_packet_gates.inc.c
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_assert_condition_authority_gates.inc.c
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_assert_condition_pipeline_gates.inc.c
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_assert_condition_initialization.inc.c
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_assert_condition_lexical_gates.inc.c
    VERBATIM)
add_executable(test_xir_assert_condition_native ${CMAKE_CURRENT_SOURCE_DIR}/xir/test_xir_assert_condition_native.c ${XIR_ASSERT_CONDITION_C})
target_link_libraries(test_xir_assert_condition_native PRIVATE xray_xir_source xray_xir_vm)
foreach(assert_target IN ITEMS test_xir_assert_condition test_xir_assert_condition_native)
    if(MSVC)
        target_compile_options(${assert_target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${assert_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_xir_assert_condition COMMAND test_xir_assert_condition
    ${XIR_ASSERT_CONDITION_DIR} ${XIR_ASSERT_CONDITION_TEST_C})
add_test(NAME test_xir_assert_condition_native COMMAND test_xir_assert_condition_native)
add_test(NAME test_xir_assert_condition_wire_vector COMMAND ${XRAY_PYTHON}
    ${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_assert_condition_vector.py)
set_tests_properties(test_xir_assert_condition test_xir_assert_condition_native
    test_xir_assert_condition_wire_vector PROPERTIES LABELS "unit;xir;execution;ownership;abi;assertion")
