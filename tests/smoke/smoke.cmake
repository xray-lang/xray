add_test(NAME smoke
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/smoke/run_smoke.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --output ${CMAKE_CURRENT_BINARY_DIR}/smoke)
set_tests_properties(smoke PROPERTIES
    TIMEOUT 300 PROCESSORS 1 LABELS "smoke;fast;cli;usability")

add_test(NAME smoke_runner
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/smoke/test_runner.py)
set_tests_properties(smoke_runner PROPERTIES
    TIMEOUT 30 PROCESSORS 1 LABELS "smoke;fast;cli;usability")
