

add_test(NAME test_xir_p09_sizing75_vectors COMMAND ${XRAY_PYTHON}
    "${PROJECT_SOURCE_DIR}/tests/unit/xir/derive_hash75_vectors.py"
)
set_tests_properties(test_xir_p09_sizing75_vectors PROPERTIES TIMEOUT 120 LABELS "unit;xir;wire;independent;compiler")
