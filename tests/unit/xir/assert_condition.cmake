# The declaration source and default owner are consumed by VM and real C.
set(XIR_ASSERT_CONDITION_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/assert-condition-source)
set(XIR_ASSERT_CONDITION_C ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_assert_condition.c)
set(XIR_ASSERT_CONDITION_TEST_C ${CMAKE_CURRENT_BINARY_DIR}/assert-condition-test.c)
file(MAKE_DIRECTORY ${XIR_ASSERT_CONDITION_DIR})

add_test(NAME test_xir_assert_condition_wire_vector COMMAND ${XRAY_PYTHON}
    ${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_assert_condition_vector.py)
set_tests_properties(test_xir_assert_condition_wire_vector PROPERTIES LABELS "unit;xir;execution;ownership;abi;assertion")
