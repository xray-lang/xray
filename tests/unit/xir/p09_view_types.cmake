

add_test(NAME test_xir_p09_view85_field_oracle COMMAND ${XRAY_PYTHON}
    ${CMAKE_CURRENT_LIST_DIR}/derive_view85_vectors.py)
set_tests_properties(test_xir_p09_view85_field_oracle PROPERTIES TIMEOUT 30 LABELS "unit;xir;codec;ownership")
