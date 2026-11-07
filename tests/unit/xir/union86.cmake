

add_test(NAME test_xir_union86_vectors COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_LIST_DIR}/derive_union86_vectors.py)
add_test(NAME test_xir_union86_retired_vectors COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_LIST_DIR}/derive_union86_retired.py)
set_tests_properties(test_xir_union86_vectors test_xir_union86_retired_vectors PROPERTIES TIMEOUT 120 LABELS "unit;xir;checked;independent;ownership")
