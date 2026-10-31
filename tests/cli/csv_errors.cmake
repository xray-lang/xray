if(WIN32 AND TARGET xray)
    add_test(NAME test_csv_typed_errors
        COMMAND ${XRAY_PYTHON} -X utf8 "${PROJECT_SOURCE_DIR}/tests/cli/test_csv_errors.py"
            --root "${PROJECT_SOURCE_DIR}" --binary $<TARGET_FILE:xray>
            --output "${CMAKE_BINARY_DIR}/csv-typed-errors")
    set_tests_properties(test_csv_typed_errors PROPERTIES
        TIMEOUT 120 RUN_SERIAL TRUE LABELS "cli;stdlib;csv;errors;native")
endif()
