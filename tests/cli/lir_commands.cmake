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

add_test(NAME test_lir_cli_inline_class_array_push
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_invoke_try_assert.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --fixtures tests/fixtures/lir_cli/inline_class_array_push --compiler "${CMAKE_C_COMPILER}"
        --output ${PROJECT_BINARY_DIR}/lir-cli-inline-class-array-push)
set_tests_properties(test_lir_cli_inline_class_array_push PROPERTIES
    TIMEOUT 180 PROCESSORS 1 LABELS "cli;lir;native;ownership")

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

add_test(NAME test_lir_cli_integer_bits
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_invoke_try_assert.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --fixtures tests/fixtures/lir_cli/integer_bits --compiler "${CMAKE_C_COMPILER}"
        --output ${PROJECT_BINARY_DIR}/lir-cli-integer-bits)
set_tests_properties(test_lir_cli_integer_bits PROPERTIES
    TIMEOUT 180 PROCESSORS 1 LABELS "cli;lir;native")

add_test(NAME test_lir_cli_math_rounding
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_invoke_try_assert.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --fixtures tests/fixtures/lir_cli/math_rounding --compiler "${CMAKE_C_COMPILER}"
        --output ${PROJECT_BINARY_DIR}/lir-cli-math-rounding)
set_tests_properties(test_lir_cli_math_rounding PROPERTIES
    TIMEOUT 180 PROCESSORS 1 LABELS "cli;lir;native")

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

add_test(NAME test_lir_cli_invoke_owned_array
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_invoke_try_assert.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --fixtures tests/fixtures/lir_cli/invoke_owned_array --compiler "${CMAKE_C_COMPILER}"
        --output ${PROJECT_BINARY_DIR}/lir-cli-invoke-owned-array)
set_tests_properties(test_lir_cli_invoke_owned_array PROPERTIES
    TIMEOUT 180 PROCESSORS 1 LABELS "cli;lir;native;ownership")

add_test(NAME test_lir_cli_invoke_string_argument
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_invoke_try_assert.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --fixtures tests/fixtures/lir_cli/invoke_string_argument --compiler "${CMAKE_C_COMPILER}"
        --output ${PROJECT_BINARY_DIR}/lir-cli-invoke-string-argument)
set_tests_properties(test_lir_cli_invoke_string_argument PROPERTIES
    TIMEOUT 180 PROCESSORS 1 LABELS "cli;lir;native;ownership")

add_test(NAME test_lir_cli_invoke_catch
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_invoke_try_assert.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --fixtures tests/fixtures/lir_cli/invoke_catch --compiler "${CMAKE_C_COMPILER}"
        --output ${PROJECT_BINARY_DIR}/lir-cli-invoke-catch)
set_tests_properties(test_lir_cli_invoke_catch PROPERTIES
    TIMEOUT 180 PROCESSORS 1 LABELS "cli;lir;native;ownership")

add_test(NAME test_lir_cli_invoke_catch_local
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_invoke_try_assert.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --fixtures tests/fixtures/lir_cli/invoke_catch_local --compiler "${CMAKE_C_COMPILER}"
        --output ${PROJECT_BINARY_DIR}/lir-cli-invoke-catch-local)
set_tests_properties(test_lir_cli_invoke_catch_local PROPERTIES
    TIMEOUT 180 PROCESSORS 1 LABELS "cli;lir;native;ownership")

add_test(NAME test_lir_cli_invoke_borrowed_slice
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_invoke_try_assert.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --fixtures tests/fixtures/lir_cli/invoke_borrowed_slice --compiler "${CMAKE_C_COMPILER}"
        --output ${PROJECT_BINARY_DIR}/lir-cli-invoke-borrowed-slice)
set_tests_properties(test_lir_cli_invoke_borrowed_slice PROPERTIES
    TIMEOUT 180 PROCESSORS 1 LABELS "cli;lir;native;ownership")

add_test(NAME test_lir_cli_invoke_strict_utf8
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_invoke_try_assert.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --fixtures tests/fixtures/lir_cli/invoke_strict_utf8 --compiler "${CMAKE_C_COMPILER}"
        --output ${PROJECT_BINARY_DIR}/lir-cli-invoke-strict-utf8)
set_tests_properties(test_lir_cli_invoke_strict_utf8 PROPERTIES
    TIMEOUT 180 PROCESSORS 1 LABELS "cli;lir;native;ownership")

add_test(NAME test_lir_cli_lossy_utf8
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_invoke_try_assert.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --fixtures tests/fixtures/lir_cli/lossy_utf8 --compiler "${CMAKE_C_COMPILER}"
        --output ${PROJECT_BINARY_DIR}/lir-cli-lossy-utf8)
set_tests_properties(test_lir_cli_lossy_utf8 PROPERTIES
    TIMEOUT 180 PROCESSORS 1 LABELS "cli;lir;native;ownership")

add_test(NAME test_lir_cli_class_optional_i64
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_invoke_try_assert.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --fixtures tests/fixtures/lir_cli/class_optional_i64 --compiler "${CMAKE_C_COMPILER}"
        --output ${PROJECT_BINARY_DIR}/lir-cli-class-optional-i64)
set_tests_properties(test_lir_cli_class_optional_i64 PROPERTIES
    TIMEOUT 180 PROCESSORS 1 LABELS "cli;lir;native;ownership")

add_test(NAME test_lir_cli_class_string_fields
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_invoke_try_assert.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --fixtures tests/fixtures/lir_cli/class_string_fields --compiler "${CMAKE_C_COMPILER}"
        --output ${PROJECT_BINARY_DIR}/lir-cli-class-string-fields)
set_tests_properties(test_lir_cli_class_string_fields PROPERTIES
    TIMEOUT 180 PROCESSORS 1 LABELS "cli;lir;native;ownership")

add_test(NAME test_lir_cli_class_method_calls
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_invoke_try_assert.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --fixtures tests/fixtures/lir_cli/class_method_calls --compiler "${CMAKE_C_COMPILER}"
        --output ${PROJECT_BINARY_DIR}/lir-cli-class-method-calls)
set_tests_properties(test_lir_cli_class_method_calls PROPERTIES
    TIMEOUT 180 PROCESSORS 1 LABELS "cli;lir;native;ownership")

add_test(NAME test_lir_cli_f64_try_parse
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_invoke_try_assert.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --fixtures tests/fixtures/lir_cli/f64_try_parse --compiler "${CMAKE_C_COMPILER}"
        --output ${PROJECT_BINARY_DIR}/lir-cli-f64-try-parse)
set_tests_properties(test_lir_cli_f64_try_parse PROPERTIES
    TIMEOUT 180 PROCESSORS 1 LABELS "cli;lir;native;ownership")

add_test(NAME test_lir_cli_bool_try_parse
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_invoke_try_assert.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --fixtures tests/fixtures/lir_cli/bool_try_parse --compiler "${CMAKE_C_COMPILER}"
        --output ${PROJECT_BINARY_DIR}/lir-cli-bool-try-parse)
set_tests_properties(test_lir_cli_bool_try_parse PROPERTIES
    TIMEOUT 180 PROCESSORS 1 LABELS "cli;lir;native;ownership")

add_test(NAME test_lir_cli_string_test
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_invoke_try_assert.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --fixtures tests/fixtures/lir_cli/string_test --compiler "${CMAKE_C_COMPILER}"
        --output ${PROJECT_BINARY_DIR}/lir-cli-string-test)
set_tests_properties(test_lir_cli_string_test PROPERTIES
    TIMEOUT 180 PROCESSORS 1 LABELS "cli;lir;native;ownership")

add_test(NAME test_lir_cli_string_last_index_of
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_invoke_try_assert.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --fixtures tests/fixtures/lir_cli/string_last_index_of --compiler "${CMAKE_C_COMPILER}"
        --output ${PROJECT_BINARY_DIR}/lir-cli-string-last-index-of)
set_tests_properties(test_lir_cli_string_last_index_of PROPERTIES
    TIMEOUT 180 PROCESSORS 1 LABELS "cli;lir;native;ownership")

add_test(NAME test_lir_cli_string_order
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_invoke_try_assert.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --fixtures tests/fixtures/lir_cli/string_order --compiler "${CMAKE_C_COMPILER}"
        --output ${PROJECT_BINARY_DIR}/lir-cli-string-order)
set_tests_properties(test_lir_cli_string_order PROPERTIES
    TIMEOUT 180 PROCESSORS 1 LABELS "cli;lir;native;ownership")

add_test(NAME test_lir_cli_string_replace
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_invoke_try_assert.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --fixtures tests/fixtures/lir_cli/string_replace --compiler "${CMAKE_C_COMPILER}"
        --output ${PROJECT_BINARY_DIR}/lir-cli-string-replace)
set_tests_properties(test_lir_cli_string_replace PROPERTIES
    TIMEOUT 180 PROCESSORS 1 LABELS "cli;lir;native;ownership")

add_test(NAME test_lir_cli_literal_uses
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_invoke_try_assert.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --fixtures tests/fixtures/lir_cli/literal_uses --compiler "${CMAKE_C_COMPILER}"
        --output ${PROJECT_BINARY_DIR}/lir-cli-literal-uses)
set_tests_properties(test_lir_cli_literal_uses PROPERTIES
    TIMEOUT 180 PROCESSORS 1 LABELS "cli;lir;native;ownership")

add_test(NAME test_lir_cli_array_capacity
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_invoke_try_assert.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --fixtures tests/fixtures/lir_cli/array_capacity --compiler "${CMAKE_C_COMPILER}"
        --output ${PROJECT_BINARY_DIR}/lir-cli-array-capacity)
set_tests_properties(test_lir_cli_array_capacity PROPERTIES
    TIMEOUT 180 PROCESSORS 1 LABELS "cli;lir;native;ownership")

add_test(NAME test_lir_cli_struct_align
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_invoke_try_assert.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --fixtures tests/fixtures/lir_cli/struct_align --compiler "${CMAKE_C_COMPILER}"
        --output ${PROJECT_BINARY_DIR}/lir-cli-struct-align)
set_tests_properties(test_lir_cli_struct_align PROPERTIES
    TIMEOUT 180 PROCESSORS 1 LABELS "cli;lir;native;ownership")

# The temporary-path transports exist only on Windows: the Unix boundary
# reports the primitive unavailable, which is a legitimate null result.
if(WIN32)
    add_test(NAME test_lir_cli_host_temp_directory
        COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_invoke_try_assert.py
            --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
            --fixtures tests/fixtures/lir_cli/host_temp_directory --compiler "${CMAKE_C_COMPILER}"
            --output ${PROJECT_BINARY_DIR}/lir-cli-host-temp-directory)
    set_tests_properties(test_lir_cli_host_temp_directory PROPERTIES
        TIMEOUT 180 PROCESSORS 1 LABELS "cli;lir;native;host")
    add_test(NAME test_lir_cli_host_temp_file
        COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_invoke_try_assert.py
            --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
            --fixtures tests/fixtures/lir_cli/host_temp_file --compiler "${CMAKE_C_COMPILER}"
            --output ${PROJECT_BINARY_DIR}/lir-cli-host-temp-file)
    set_tests_properties(test_lir_cli_host_temp_file PROPERTIES
        TIMEOUT 180 PROCESSORS 1 LABELS "cli;lir;native;host")
endif()
if((WIN32 AND XR_TARGET_ARCH MATCHES "^(x86_64|AMD64|amd64)$") OR XR_XIR_POSIX_SDK_ARCHITECTURE)
    add_test(NAME test_lir_cli_host_links
        COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_invoke_try_assert.py
            --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
            --fixtures tests/fixtures/lir_cli/host_links --compiler "${CMAKE_C_COMPILER}"
            --output ${PROJECT_BINARY_DIR}/lir-cli-host-links)
    set_tests_properties(test_lir_cli_host_links PROPERTIES
        TIMEOUT 180 PROCESSORS 1 LABELS "cli;lir;native;host")
endif()

add_test(NAME test_lir_cli_top_const_narrow
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_invoke_try_assert.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --fixtures tests/fixtures/lir_cli/top_const_narrow --compiler "${CMAKE_C_COMPILER}"
        --output ${PROJECT_BINARY_DIR}/lir-cli-top-const-narrow)
set_tests_properties(test_lir_cli_top_const_narrow PROPERTIES
    TIMEOUT 180 PROCESSORS 1 LABELS "cli;lir;native;narrowing")

add_test(NAME test_lir_cli_host_method_import
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_invoke_try_assert.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --fixtures tests/fixtures/lir_cli/host_method_import --compiler "${CMAKE_C_COMPILER}"
        --output ${PROJECT_BINARY_DIR}/lir-cli-host-method-import)
set_tests_properties(test_lir_cli_host_method_import PROPERTIES
    TIMEOUT 180 PROCESSORS 1 LABELS "cli;lir;native;ownership")

add_test(NAME test_cli_method_import_visibility
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_method_import_visibility_tests.py
        --binary $<TARGET_FILE:xray> --output ${PROJECT_BINARY_DIR}/method-import-visibility)
set_tests_properties(test_cli_method_import_visibility PROPERTIES
    TIMEOUT 180 PROCESSORS 1 LABELS "cli;lir;ownership")

foreach(enum_profile IN ITEMS class_enum_field enum_equality)
    add_test(NAME test_lir_cli_${enum_profile}
        COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_invoke_try_assert.py
            --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
            --fixtures tests/fixtures/lir_cli/${enum_profile} --compiler "${CMAKE_C_COMPILER}"
            --output ${PROJECT_BINARY_DIR}/lir-cli-${enum_profile})
    set_tests_properties(test_lir_cli_${enum_profile} PROPERTIES
        TIMEOUT 180 PROCESSORS 1 LABELS "cli;lir;native;ownership")
endforeach()

foreach(defer_profile IN ITEMS sync lowering source propagation panic caught)
    add_test(NAME test_lir_cli_defer_${defer_profile}
        COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_invoke_try_assert.py
            --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
            --fixtures tests/fixtures/lir_cli/invoke_defer/${defer_profile} --compiler "${CMAKE_C_COMPILER}"
            --output ${PROJECT_BINARY_DIR}/lir-cli-defer-${defer_profile})
    set_tests_properties(test_lir_cli_defer_${defer_profile} PROPERTIES
        TIMEOUT 180 PROCESSORS 1 LABELS "cli;lir;native;ownership")
endforeach()

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

add_test(NAME test_lir_cli_host_directory
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_lir_host_directory.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --output ${PROJECT_BINARY_DIR}/lir-cli-host-directory)
set_tests_properties(test_lir_cli_host_directory PROPERTIES
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
