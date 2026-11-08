add_test(NAME test_xir_tuple62_vectors COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_tuple62_vectors.py)
add_test(NAME test_xir_tuple63_vectors COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_tuple63_vectors.py)
add_test(NAME test_xir_tuple63_readers COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/xir/verify_tuple63_readers.py
    --reader $<TARGET_FILE:test_xir_nested_nullable_reader> --output ${CMAKE_CURRENT_BINARY_DIR}/tuple63-reader-probes)
add_test(NAME test_xir_tuple63_literal_projection COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_tuple63_migration.py)
set_tests_properties(test_xir_tuple62_vectors test_xir_tuple63_vectors test_xir_tuple63_readers test_xir_tuple63_literal_projection PROPERTIES LABELS "unit;xir;abi;ownership" TIMEOUT 300)

add_test(NAME test_xir_tuple62_sizing_vectors COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_tuple62_migration.py)
set_tests_properties(test_xir_tuple62_sizing_vectors PROPERTIES LABELS "unit;xir;abi;ownership" TIMEOUT 300)
