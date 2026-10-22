add_test(NAME test_source_entry
    COMMAND ${XRAY_PYTHON} -X utf8 ${CMAKE_CURRENT_LIST_DIR}/source_entry.py
        --binary $<TARGET_FILE:xray> --library-producer $<TARGET_FILE:xir_stdlib_checkedgen>
        --root ${PROJECT_SOURCE_DIR} --output ${CMAKE_CURRENT_BINARY_DIR}/source-entry)
set_tests_properties(test_source_entry PROPERTIES
    TIMEOUT 120 LABELS "unit;xir;source;language;modules;diagnostics")
