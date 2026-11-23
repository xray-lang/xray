add_test(NAME test_lir_cli_commands
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_commands.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --output ${PROJECT_BINARY_DIR}/lir-cli-commands
        --runtime-library $<TARGET_FILE:xray_native_runtime>)
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
    TIMEOUT 180 PROCESSORS 2 RUN_SERIAL TRUE LABELS "cli;lir;native;ownership")

add_test(NAME test_lir_cli_string_slice
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_string_slice.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --output ${PROJECT_BINARY_DIR}/lir-cli-string-slice)
set_tests_properties(test_lir_cli_string_slice PROPERTIES
    TIMEOUT 180 PROCESSORS 1 LABELS "cli;lir;native;ownership")

add_test(NAME test_lir_cli_string_index_of
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_string_index_of.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --output ${PROJECT_BINARY_DIR}/lir-cli-string-index-of)
set_tests_properties(test_lir_cli_string_index_of PROPERTIES
    TIMEOUT 180 PROCESSORS 1 LABELS "cli;lir;native;ownership")

add_test(NAME test_lir_cli_assertions
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_assertions.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --output ${PROJECT_BINARY_DIR}/lir-cli-assertions)
set_tests_properties(test_lir_cli_assertions PROPERTIES
    TIMEOUT 180 PROCESSORS 1 RUN_SERIAL TRUE LABELS "cli;lir;native;ownership")

add_test(NAME test_lir_cli_invoke_try_assert
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_invoke_try_assert.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --output ${PROJECT_BINARY_DIR}/lir-cli-invoke-try-assert)
set_tests_properties(test_lir_cli_invoke_try_assert PROPERTIES
    TIMEOUT 180 PROCESSORS 1 LABELS "cli;lir;native;ownership")

add_test(NAME test_lir_cli_invoke_try_propagate
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_invoke_try_assert.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --fixtures tests/fixtures/lir_cli/invoke_try_propagate
        --output ${PROJECT_BINARY_DIR}/lir-cli-invoke-try-propagate)
set_tests_properties(test_lir_cli_invoke_try_propagate PROPERTIES
    TIMEOUT 180 PROCESSORS 1 LABELS "cli;lir;native;ownership")

add_test(NAME test_lir_cli_invoke_try_optional
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_invoke_try_assert.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --fixtures tests/fixtures/lir_cli/invoke_try_optional --compiler "${CMAKE_C_COMPILER}"
        --output ${PROJECT_BINARY_DIR}/lir-cli-invoke-try-optional)
set_tests_properties(test_lir_cli_invoke_try_optional PROPERTIES
    TIMEOUT 180 PROCESSORS 1 LABELS "cli;lir;native;ownership")

add_test(NAME test_lir_cli_invoke_managed_string
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_invoke_try_assert.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --fixtures tests/fixtures/lir_cli/invoke_managed_string --compiler "${CMAKE_C_COMPILER}"
        --output ${PROJECT_BINARY_DIR}/lir-cli-invoke-managed-string)
set_tests_properties(test_lir_cli_invoke_managed_string PROPERTIES
    TIMEOUT 180 PROCESSORS 1 LABELS "cli;lir;native;ownership")

add_test(NAME test_lir_cli_invoke_init_root
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_invoke_try_assert.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --fixtures tests/fixtures/lir_cli/invoke_init_root --compiler "${CMAKE_C_COMPILER}"
        --output ${PROJECT_BINARY_DIR}/lir-cli-invoke-init-root)
set_tests_properties(test_lir_cli_invoke_init_root PROPERTIES
    TIMEOUT 180 PROCESSORS 1 LABELS "cli;lir;native;ownership")

add_test(NAME test_lir_cli_string_byte_length
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_string_byte_length.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --output ${PROJECT_BINARY_DIR}/lir-cli-string-byte-length)
set_tests_properties(test_lir_cli_string_byte_length PROPERTIES
    TIMEOUT 180 PROCESSORS 1 LABELS "cli;lir;native;ownership")

add_test(NAME test_lir_cli_host_path
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_host_path.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --output ${PROJECT_BINARY_DIR}/lir-cli-host-path)
set_tests_properties(test_lir_cli_host_path PROPERTIES
    TIMEOUT 180 PROCESSORS 1 LABELS "cli;lir;native;ownership")

add_test(NAME test_lir_cli_host_file_write
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_host_file_write.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --output ${PROJECT_BINARY_DIR}/lir-cli-host-file-write)
set_tests_properties(test_lir_cli_host_file_write PROPERTIES
    TIMEOUT 180 PROCESSORS 1 LABELS "cli;lir;native;ownership")

add_test(NAME test_lir_cli_target_platform
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_target_platform.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --output ${PROJECT_BINARY_DIR}/lir-cli-target-platform)
set_tests_properties(test_lir_cli_target_platform PROPERTIES
    TIMEOUT 180 PROCESSORS 1 LABELS "cli;lir;native;ownership")

add_test(NAME test_lir_cli_host_environment
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_host_environment.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --compiler "${CMAKE_C_COMPILER}" --output ${PROJECT_BINARY_DIR}/lir-cli-host-environment)
set_tests_properties(test_lir_cli_host_environment PROPERTIES
    TIMEOUT 180 PROCESSORS 1 LABELS "cli;lir;native;ownership")
