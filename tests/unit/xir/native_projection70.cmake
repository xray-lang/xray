# Independently encoded current70 policy with authentic65/69 preimages retained.
add_test(NAME test_xir_native_projection70_vectors COMMAND ${XRAY_PYTHON}
    ${CMAKE_CURRENT_LIST_DIR}/derive_native_projection70_vectors.py)
set_tests_properties(test_xir_native_projection70_vectors PROPERTIES LABELS "unit;xir;abi;native;ownership;identity;generated-c")

# The current single identity owns current macros and every complete prior frame.
add_test(NAME test_xir_native_projection72_vectors COMMAND ${XRAY_PYTHON}
    ${CMAKE_CURRENT_LIST_DIR}/derive_native_projection72_vectors.py)
set_tests_properties(test_xir_native_projection72_vectors PROPERTIES
    LABELS "unit;xir;abi;native;ownership;identity;generated-c")
