

add_test(NAME source_birth_storage_vectors COMMAND "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/unit/xir/derive_combined_wire.py"
    --source-root "${PROJECT_SOURCE_DIR}")
add_test(NAME checked_storage76_vectors COMMAND "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/unit/xir/derive_combined_wire.py"
    --source-root "${PROJECT_SOURCE_DIR}")
add_test(NAME checked_sizing76_vectors COMMAND "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/unit/xir/derive_combined_wire.py"
    --source-root "${PROJECT_SOURCE_DIR}")
set_tests_properties(source_birth_storage_vectors checked_storage76_vectors checked_sizing76_vectors
    PROPERTIES TIMEOUT 120 LABELS "unit;api;source;generation;checked;storage;independent")
