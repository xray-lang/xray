# Include after nested_nullable.cmake; its generic real reader already owns a
# finite compile context and checks failed-output absence and return to the live ledger baseline.
add_test(NAME test_xir_semantic60_vectors COMMAND ${XRAY_PYTHON}
    ${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_semantic60_migration.py)
set_tests_properties(test_xir_semantic60_vectors PROPERTIES LABELS "unit;xir;abi" TIMEOUT 30)
add_test(NAME test_xir_semantic61_readers COMMAND ${XRAY_PYTHON}
    ${CMAKE_CURRENT_SOURCE_DIR}/xir/verify_semantic61_readers.py
    --reader $<TARGET_FILE:test_xir_nested_nullable_reader>
    --output ${CMAKE_CURRENT_BINARY_DIR}/semantic61-reader-probes)
set_tests_properties(test_xir_semantic61_readers PROPERTIES LABELS "unit;xir;abi;ownership" TIMEOUT 300)

add_test(NAME test_xir_semantic61_vectors COMMAND ${XRAY_PYTHON}
    ${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_semantic61_migration.py)
set_tests_properties(test_xir_semantic61_vectors PROPERTIES LABELS "unit;xir;abi" TIMEOUT 30)
