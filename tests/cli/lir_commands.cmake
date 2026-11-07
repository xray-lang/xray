add_test(NAME test_lir_cli_commands
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_commands.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --output ${PROJECT_BINARY_DIR}/lir-cli-commands
        --runtime-library $<TARGET_FILE:xray_native_runtime>
        --runtime-library $<TARGET_FILE:xray_xir_scalar>
        --runtime-library $<TARGET_FILE:xray_xir_admission>
        --runtime-library $<TARGET_FILE:xray_xir_declarations>
        --runtime-library $<TARGET_FILE:xray_compile_resources>)
set_tests_properties(test_lir_cli_commands PROPERTIES
    TIMEOUT 180 PROCESSORS 1 RUN_SERIAL TRUE LABELS "cli;lir;native;ownership")
