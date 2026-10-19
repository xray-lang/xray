add_test(NAME test_xir_public_cli_checked_families
    COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/cli/run_xir_public_cli_checked_families.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR})
set_tests_properties(test_xir_public_cli_checked_families PROPERTIES
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    LABELS "cli;xir;stdlib;native;execution;ownership")
add_test(NAME test_xir_public_cli_library_calls
    COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/cli/run_xir_public_cli_library_calls.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR})
set_tests_properties(test_xir_public_cli_library_calls PROPERTIES
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1 WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    LABELS "cli;xir;stdlib;native;execution;ownership;ctfe")
add_test(NAME test_xir_public_cli_array_mutation_calls
    COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/cli/run_xir_public_cli_array_mutation_calls.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR})
set_tests_properties(test_xir_public_cli_array_mutation_calls PROPERTIES
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1 WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    LABELS "cli;xir;stdlib;native;execution;ownership;ctfe")
foreach(mode IN ITEMS measure faults)
    if(mode STREQUAL "measure")
        set(array_mutation_binding_mode binding-measure)
    else()
        set(array_mutation_binding_mode faults)
    endif()
    add_test(NAME test_xir_array_mutation_calls_preparation_${mode}
        COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/unit/xir/array_mutation_call_resources.py
            $<TARGET_FILE:test_xir_stdlib_preparation> ${PROJECT_SOURCE_DIR} ${mode})
    add_test(NAME test_xir_array_mutation_calls_binding_${mode}
        COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/unit/xir/array_mutation_call_resources.py
            $<TARGET_FILE:test_xir_stdlib_resources> ${PROJECT_SOURCE_DIR} ${array_mutation_binding_mode})
    set_tests_properties(test_xir_array_mutation_calls_preparation_${mode} test_xir_array_mutation_calls_binding_${mode} PROPERTIES
        TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1 LABELS "unit;xir;stdlib;resources;ownership;ctfe")
endforeach()
add_test(NAME test_xir_public_cli_array_concat_calls
    COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/cli/run_xir_public_cli_array_concat_calls.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR})
set_tests_properties(test_xir_public_cli_array_concat_calls PROPERTIES
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1 WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    LABELS "cli;xir;stdlib;native;execution;ownership;ctfe")
foreach(mode IN ITEMS measure faults)
    if(mode STREQUAL "measure")
        set(array_concat_binding_mode binding-measure)
    else()
        set(array_concat_binding_mode faults)
    endif()
    add_test(NAME test_xir_array_concat_calls_preparation_${mode}
        COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/unit/xir/array_concat_call_resources.py
            $<TARGET_FILE:test_xir_stdlib_preparation> ${PROJECT_SOURCE_DIR} ${mode})
    add_test(NAME test_xir_array_concat_calls_binding_${mode}
        COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/unit/xir/array_concat_call_resources.py
            $<TARGET_FILE:test_xir_stdlib_resources> ${PROJECT_SOURCE_DIR} ${array_concat_binding_mode})
    set_tests_properties(test_xir_array_concat_calls_preparation_${mode} test_xir_array_concat_calls_binding_${mode} PROPERTIES
        TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1 LABELS "unit;xir;stdlib;resources;ownership;ctfe")
endforeach()
add_test(NAME test_xir_public_cli_array_callback_calls
    COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/cli/run_xir_public_cli_array_callback_calls.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR})
set_tests_properties(test_xir_public_cli_array_callback_calls PROPERTIES
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1 WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    LABELS "cli;xir;stdlib;native;execution;ownership;ctfe")
foreach(mode IN ITEMS measure faults)
    if(mode STREQUAL "measure")
        set(array_callback_binding_mode binding-measure)
    else()
        set(array_callback_binding_mode faults)
    endif()
    add_test(NAME test_xir_array_callback_calls_preparation_${mode}
        COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/unit/xir/array_callback_call_resources.py
            $<TARGET_FILE:test_xir_stdlib_preparation> ${PROJECT_SOURCE_DIR} ${mode})
    add_test(NAME test_xir_array_callback_calls_binding_${mode}
        COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/unit/xir/array_callback_call_resources.py
            $<TARGET_FILE:test_xir_stdlib_resources> ${PROJECT_SOURCE_DIR} ${array_callback_binding_mode})
    set_tests_properties(test_xir_array_callback_calls_preparation_${mode} test_xir_array_callback_calls_binding_${mode} PROPERTIES
        TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1 LABELS "unit;xir;stdlib;resources;ownership;ctfe")
endforeach()
