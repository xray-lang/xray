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

add_test(NAME test_lir_cli_effects
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_commands.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --output ${PROJECT_BINARY_DIR}/lir-cli-commands --effects-only)
set_tests_properties(test_lir_cli_effects PROPERTIES
    TIMEOUT 60 PROCESSORS 1 LABELS "cli;effects;frontend;regression")

add_test(NAME test_lir_cli_module_state
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_module_state.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --output ${PROJECT_BINARY_DIR}/lir-cli-module-state)
set_tests_properties(test_lir_cli_module_state PROPERTIES
    TIMEOUT 180 PROCESSORS 1 RUN_SERIAL TRUE LABELS "cli;lir;native;ownership")

add_test(NAME test_lir_cli_array_native
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_array_native.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --output ${PROJECT_BINARY_DIR}/lir-cli-array-native)
set_tests_properties(test_lir_cli_array_native PROPERTIES
    TIMEOUT 180 PROCESSORS 1 RUN_SERIAL TRUE LABELS "cli;lir;native;ownership")
