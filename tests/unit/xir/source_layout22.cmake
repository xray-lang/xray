# Lowered layout identities are framed independently of the artifact producer.
add_test(NAME test_xir_source_layout22_identity COMMAND ${XRAY_PYTHON}
    ${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_source_layout22_identity.py)
set_tests_properties(test_xir_source_layout22_identity
    PROPERTIES LABELS "unit;xir;abi;ownership;identity;lowered")
