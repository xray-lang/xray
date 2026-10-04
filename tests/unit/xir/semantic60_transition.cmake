# Include after nested_nullable.cmake; its generic real reader already owns a
# finite compile context and checks failed-output absence and return to the live ledger baseline.
add_test(NAME test_xir_semantic60_vectors COMMAND ${XRAY_PYTHON}
    ${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_semantic60_migration.py)
set_tests_properties(test_xir_semantic60_vectors PROPERTIES LABELS "unit;xir;abi" TIMEOUT 30)
add_test(NAME test_xir_semantic60_readers COMMAND ${XRAY_PYTHON}
    ${CMAKE_CURRENT_SOURCE_DIR}/xir/verify_semantic60_readers.py
    --reader $<TARGET_FILE:test_xir_nested_nullable_reader>
    --output ${CMAKE_CURRENT_BINARY_DIR}/semantic60-reader-probes)
set_tests_properties(test_xir_semantic60_readers PROPERTIES LABELS "unit;xir;abi;ownership" TIMEOUT 300)
