# Source, wire, runtime and both native/VM directions use one equality owner.
set(XIR_ASSERT_EQUAL_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/assert-equal-source)
set(XIR_ASSERT_EQUAL_C ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_assert_equal.c)
file(MAKE_DIRECTORY ${XIR_ASSERT_EQUAL_DIR})

add_test(NAME test_xir_assert_equal_wire_vector COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_assert_equal_vector.py)
add_test(NAME test_xir_equal_wire_migration COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_equal57_migration.py)
set_tests_properties(test_xir_assert_equal_wire_vector test_xir_equal_wire_migration PROPERTIES LABELS "unit;xir;execution;ownership;abi;assertion;equality")
