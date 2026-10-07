# Complete current policy preimages are independent of the projection producer.
add_test(NAME test_xir_native_projection69_vectors COMMAND ${XRAY_PYTHON}
    ${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_native_projection69_vectors.py)
set_tests_properties(test_xir_native_projection69_vectors
    PROPERTIES LABELS "unit;xir;abi;ownership;identity;generated-c")
