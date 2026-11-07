add_test(NAME test_xir_p09_map_index77_vectors COMMAND ${XRAY_PYTHON}
    "${PROJECT_SOURCE_DIR}/tests/unit/xir/derive_map_index77_vectors.py"
    --source "${PROJECT_SOURCE_DIR}" --output "${CMAKE_BINARY_DIR}/p09-map-index77-vectors")
set_tests_properties(test_xir_p09_map_index77_vectors PROPERTIES TIMEOUT 30 LABELS "unit;xir;hash;codec")
