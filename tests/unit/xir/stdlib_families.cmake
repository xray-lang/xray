add_test(NAME test_xir_public_cli_checked_families
    COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/cli/run_xir_public_cli_checked_families.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR})
set_tests_properties(test_xir_public_cli_checked_families PROPERTIES
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    LABELS "cli;xir;stdlib;native;execution;ownership")
