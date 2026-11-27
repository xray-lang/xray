# First-use regressions run the real build-tree CLI: default stdlib discovery
# for an uninstalled (or package-configured) build and UTF-8 BOM sources.
set(_xray_cli_usability_source_tree "")
if(XRAY_SOURCE_TREE_STDLIB)
    set(_xray_cli_usability_source_tree --source-tree-stdlib "${PROJECT_SOURCE_DIR}/stdlib")
endif()
add_test(NAME test_cli_usability
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_cli_usability.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --output ${CMAKE_CURRENT_BINARY_DIR}/cli-usability
        ${_xray_cli_usability_source_tree})
set_tests_properties(test_cli_usability PROPERTIES
    TIMEOUT 120 PROCESSORS 1 LABELS "cli;usability;stdlib")

# Native executables are published on Windows x86_64 and on macOS/Linux
# x86_64 and arm64; every other host emits C only.
if((WIN32 AND XR_TARGET_ARCH MATCHES "^(x86_64|AMD64|amd64)$") OR XR_XIR_POSIX_SDK_ARCHITECTURE)
add_test(NAME test_cli_build_options
    COMMAND ${XRAY_PYTHON} -B -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_build_options.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --output ${CMAKE_CURRENT_BINARY_DIR}/cli-build-options)
set_tests_properties(test_cli_build_options PROPERTIES
    TIMEOUT 120 PROCESSORS 1 LABELS "cli;usability;build")

# A program published by `xray build` owns no resource profile, so it has to
# refuse the product options with the CLI's own exit codes instead of running
# without them; this gate also covers the build that publishes it.
add_test(NAME test_cli_resource_profiles_native
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_resource_profiles.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --output ${CMAKE_CURRENT_BINARY_DIR}/resource-profiles-native --suite native)
set_tests_properties(test_cli_resource_profiles_native PROPERTIES
    TIMEOUT 300 PROCESSORS 1 LABELS "L3;cli;resources;legacy-native")
endif()

# XIR executes on every target with 64-bit pointers and little-endian bytes.
if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_C_BYTE_ORDER STREQUAL "LITTLE_ENDIAN")
add_test(NAME test_cli_resource_profiles_vm
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_resource_profiles.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --output ${CMAKE_CURRENT_BINARY_DIR}/resource-profiles-vm --suite vm)
set_tests_properties(test_cli_resource_profiles_vm PROPERTIES
    TIMEOUT 90 PROCESSORS 1 LABELS "cli;resources;usability")
# One default run, one refusal per backend, and two long default runs that
# prove the profile is unlimited: 0.17 s for the command gates and 1.3 s for the
# large one in a Release tree, 20 s for the same large gate under ASan/UBSan.
# COST reflects the sanitizer measurement, and the 900-second floor of the
# command timeouts leaves time for owned-tree cleanup.
add_test(NAME test_cli_resource_profiles_large
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_resource_profiles.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --output ${CMAKE_CURRENT_BINARY_DIR}/resource-profiles-large --suite large)
set_tests_properties(test_cli_resource_profiles_large PROPERTIES
    TIMEOUT 900 COST 20 PROCESSORS 1 RUN_SERIAL TRUE LABELS "cli;resources;large")
# Running one discovered test still needs backend test-role startup, so that
# single product gap has its own gate instead of holding the sandbox profile
# assertions red.
add_test(NAME test_cli_resource_profiles_test_role
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_resource_profiles.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --output ${CMAKE_CURRENT_BINARY_DIR}/resource-profiles-test-role --suite test-role)
set_tests_properties(test_cli_resource_profiles_test_role PROPERTIES
    TIMEOUT 90 PROCESSORS 1 LABELS "cli;resources;test-role")

add_test(NAME test_cli_test_jobs
    COMMAND ${XRAY_PYTHON} -X utf8 ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli/run_program_test_entry_tests.py
        --xray $<TARGET_FILE:xray> -v
        ProgramTestCommand.test_files_have_isolated_instances
        ProgramTestCommand.test_file_jobs_rendezvous_and_sorted_report
        ProgramTestCommand.test_file_jobs_fail_fast_keeps_inflight_cleanup
        ProgramTestCommand.test_file_jobs_console_owns_diagnostics_in_serial_file_order
        ProgramTestCommand.test_cancelled_file_jobs_keep_prior_report
        ProgramTestCommand.test_file_jobs_keep_string_and_integer_records_whole
        ProgramTestCommand.test_file_jobs_broken_stdout_does_not_hold_record_lock)
set_tests_properties(test_cli_test_jobs PROPERTIES
    TIMEOUT 120 PROCESSORS 2 LABELS "cli;vm;concurrency;test-role")
endif()
