add_test(NAME test_xir_public_cli_checked_families
    COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/cli/run_xir_public_cli_checked_families.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR})
set_tests_properties(test_xir_public_cli_checked_families PROPERTIES
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    LABELS "cli;xir;stdlib;native;execution;ownership")

add_test(NAME test_xir_public_cli_integer_methods
    COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/cli/run_xir_public_cli_integer_methods.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR})
set_tests_properties(test_xir_public_cli_integer_methods PROPERTIES
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    LABELS "cli;xir;stdlib;native;execution;ownership;integer")

add_test(NAME test_xir_public_cli_scalar_methods
    COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/cli/run_xir_public_cli_scalar_methods.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR})
set_tests_properties(test_xir_public_cli_scalar_methods PROPERTIES
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1 WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    LABELS "cli;xir;stdlib;native;execution;ownership;scalar")

add_test(NAME test_xir_public_cli_rune_ctfe
    COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/cli/run_xir_public_cli_rune_ctfe.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR})
set_tests_properties(test_xir_public_cli_rune_ctfe PROPERTIES
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1 WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    LABELS "cli;xir;stdlib;native;execution;ownership;rune;ctfe")

add_test(NAME test_xir_public_cli_string_owned
    COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/cli/run_xir_public_cli_string_owned.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR})
set_tests_properties(test_xir_public_cli_string_owned PROPERTIES
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1 WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    LABELS "cli;xir;stdlib;native;execution;ownership;string")
add_test(NAME test_xir_public_cli_string_ctfe
    COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/cli/run_xir_public_cli_string_ctfe.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR})
set_tests_properties(test_xir_public_cli_string_ctfe PROPERTIES
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1 WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    LABELS "cli;xir;stdlib;native;execution;ownership;string;ctfe")
add_test(NAME test_xir_public_cli_float_tuple_ctfe
    COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/cli/run_xir_public_cli_float_tuple_ctfe.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR})
set_tests_properties(test_xir_public_cli_float_tuple_ctfe PROPERTIES
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1 WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    LABELS "cli;xir;stdlib;native;execution;ownership;float;tuple;ctfe")
add_test(NAME test_xir_public_cli_array_nullable_ctfe
    COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/cli/run_xir_public_cli_array_nullable_ctfe.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR})
set_tests_properties(test_xir_public_cli_array_nullable_ctfe PROPERTIES
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1 WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    LABELS "cli;xir;stdlib;native;execution;ownership;array;nullable;ctfe")
add_test(NAME test_xir_public_cli_string_append
    COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/cli/run_xir_public_cli_string_append.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR})
set_tests_properties(test_xir_public_cli_string_append PROPERTIES
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1 WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    LABELS "cli;xir;stdlib;native;execution;ownership;string")
add_test(NAME test_xir_public_cli_bounded_text_ctfe
    COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/cli/run_xir_public_cli_bounded_text_ctfe.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR})
set_tests_properties(test_xir_public_cli_bounded_text_ctfe PROPERTIES
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1 WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    LABELS "cli;xir;stdlib;execution;ownership;array;string;ctfe")
add_test(NAME test_xir_public_cli_nominal_ctfe
    COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/cli/run_xir_public_cli_nominal_ctfe.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR})
set_tests_properties(test_xir_public_cli_nominal_ctfe PROPERTIES
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1 WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    LABELS "cli;xir;stdlib;execution;ownership;nominal;ctfe")
add_test(NAME test_xir_public_cli_tuple_spread_ctfe
    COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/cli/run_xir_public_cli_tuple_spread_ctfe.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR})
set_tests_properties(test_xir_public_cli_tuple_spread_ctfe PROPERTIES
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1 LABELS "cli;xir;ctfe;tuple;spread;ownership")

add_test(NAME test_xir_public_cli_open_generic_default
    COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/cli/run_xir_public_cli_open_generic_default.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR})
set_tests_properties(test_xir_public_cli_open_generic_default PROPERTIES
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1 WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    LABELS "cli;xir;stdlib;execution;ownership;generic;ctfe")
add_executable(test_xir_string_owned_vm ${CMAKE_CURRENT_LIST_DIR}/test_xir_string_owned_vm.c)
target_include_directories(test_xir_string_owned_vm PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(test_xir_string_owned_vm PRIVATE xray_xir_stdlib_catalog xray_xir_vm)
if(MSVC)
    target_compile_options(test_xir_string_owned_vm PRIVATE /W4 /WX)
else()
    target_compile_options(test_xir_string_owned_vm PRIVATE -Wall -Wextra -Werror)
endif()
foreach(mode IN ITEMS measure faults)
    if(mode STREQUAL "measure")
        set(string_binding_mode binding-measure)
    else()
        set(string_binding_mode faults)
    endif()
    add_test(NAME test_xir_string_owned_preparation_${mode}
        COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/unit/xir/string_owned_resources.py
            $<TARGET_FILE:test_xir_stdlib_preparation> ${PROJECT_SOURCE_DIR} ${mode})
    add_test(NAME test_xir_string_owned_binding_${mode}
        COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/unit/xir/string_owned_resources.py
            $<TARGET_FILE:test_xir_stdlib_resources> ${PROJECT_SOURCE_DIR} ${string_binding_mode})
    add_test(NAME test_xir_string_owned_runtime_${mode}
        COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/unit/xir/string_owned_resources.py
            $<TARGET_FILE:test_xir_string_owned_vm> ${PROJECT_SOURCE_DIR} ${mode})
    set_tests_properties(test_xir_string_owned_preparation_${mode} test_xir_string_owned_binding_${mode} test_xir_string_owned_runtime_${mode} PROPERTIES
        TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1 LABELS "unit;xir;ownership;stdlib;resources;string")
endforeach()

add_executable(test_xir_scalar_methods_vm ${CMAKE_CURRENT_LIST_DIR}/test_xir_scalar_methods_vm.c)
target_include_directories(test_xir_scalar_methods_vm PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(test_xir_scalar_methods_vm PRIVATE xray_xir_stdlib_catalog xray_xir_vm)
if(MSVC)
    target_compile_options(test_xir_scalar_methods_vm PRIVATE /W4 /WX)
else()
    target_compile_options(test_xir_scalar_methods_vm PRIVATE -Wall -Wextra -Werror)
endif()

foreach(mode IN ITEMS measure faults)
    add_test(NAME test_xir_scalar_preparation_${mode}
        COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/unit/xir/scalar_methods_resources.py
            $<TARGET_FILE:test_xir_stdlib_preparation> ${PROJECT_SOURCE_DIR} ${mode})
    if(mode STREQUAL "measure")
        set(scalar_binding_mode binding-measure)
    else()
        set(scalar_binding_mode faults)
    endif()
    add_test(NAME test_xir_scalar_binding_${mode}
        COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/unit/xir/scalar_methods_resources.py
            $<TARGET_FILE:test_xir_stdlib_resources> ${PROJECT_SOURCE_DIR} ${scalar_binding_mode})
    add_test(NAME test_xir_scalar_runtime_${mode}
        COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/unit/xir/scalar_methods_resources.py
            $<TARGET_FILE:test_xir_scalar_methods_vm> ${PROJECT_SOURCE_DIR} ${mode})
    set_tests_properties(test_xir_scalar_preparation_${mode} test_xir_scalar_binding_${mode} test_xir_scalar_runtime_${mode} PROPERTIES
        TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1 LABELS "unit;xir;ownership;stdlib;resources;scalar")
endforeach()

include(${CMAKE_CURRENT_LIST_DIR}/integer_sqrt.cmake)
add_test(NAME test_xir_public_cli_array_control_calls
    COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/cli/run_xir_public_cli_array_control_calls.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR})
set_tests_properties(test_xir_public_cli_array_control_calls PROPERTIES
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1 WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    LABELS "cli;xir;stdlib;native;execution;ownership;ctfe")
foreach(mode IN ITEMS measure faults)
    if(mode STREQUAL "measure")
        set(array_control_binding_mode binding-measure)
    else()
        set(array_control_binding_mode faults)
    endif()
    add_test(NAME test_xir_array_control_calls_preparation_${mode}
        COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/unit/xir/array_control_call_resources.py
            $<TARGET_FILE:test_xir_stdlib_preparation> ${PROJECT_SOURCE_DIR} ${mode})
    add_test(NAME test_xir_array_control_calls_binding_${mode}
        COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/unit/xir/array_control_call_resources.py
            $<TARGET_FILE:test_xir_stdlib_resources> ${PROJECT_SOURCE_DIR} ${array_control_binding_mode})
    set_tests_properties(test_xir_array_control_calls_preparation_${mode} test_xir_array_control_calls_binding_${mode} PROPERTIES
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
add_test(NAME test_xir_public_cli_library_calls
    COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/cli/run_xir_public_cli_library_calls.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR})
set_tests_properties(test_xir_public_cli_library_calls PROPERTIES
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1 WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    LABELS "cli;xir;stdlib;native;execution;ownership;ctfe")
foreach(mode IN ITEMS measure faults)
    if(mode STREQUAL "measure")
        set(library_call_binding_mode binding-measure)
    else()
        set(library_call_binding_mode faults)
    endif()
    add_test(NAME test_xir_library_calls_preparation_${mode}
        COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/unit/xir/library_call_resources.py
            $<TARGET_FILE:test_xir_stdlib_preparation> ${PROJECT_SOURCE_DIR} ${mode})
    add_test(NAME test_xir_library_calls_binding_${mode}
        COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/unit/xir/library_call_resources.py
            $<TARGET_FILE:test_xir_stdlib_resources> ${PROJECT_SOURCE_DIR} ${library_call_binding_mode})
    set_tests_properties(test_xir_library_calls_preparation_${mode} test_xir_library_calls_binding_${mode} PROPERTIES
        TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1 LABELS "unit;xir;stdlib;resources;ownership;ctfe")
endforeach()

add_test(NAME test_xir_public_cli_integer_bits
    COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/cli/run_xir_public_cli_integer_bits.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR})
set_tests_properties(test_xir_public_cli_integer_bits PROPERTIES
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1 WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    LABELS "cli;xir;stdlib;native;execution;ownership;integer")
add_executable(test_xir_integer_bits_vm ${CMAKE_CURRENT_LIST_DIR}/test_xir_integer_bits_vm.c)
target_include_directories(test_xir_integer_bits_vm PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(test_xir_integer_bits_vm PRIVATE xray_xir_stdlib_catalog xray_xir_vm)
if(MSVC)
    target_compile_options(test_xir_integer_bits_vm PRIVATE /W4 /WX)
else()
    target_compile_options(test_xir_integer_bits_vm PRIVATE -Wall -Wextra -Werror)
endif()
foreach(mode IN ITEMS measure faults)
    if(mode STREQUAL "measure")
        set(bits_binding_mode binding-measure)
    else()
        set(bits_binding_mode faults)
    endif()
    add_test(NAME test_xir_integer_bits_preparation_${mode}
        COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/unit/xir/integer_bits_resources.py
            $<TARGET_FILE:test_xir_stdlib_preparation> ${PROJECT_SOURCE_DIR} ${mode})
    add_test(NAME test_xir_integer_bits_binding_${mode}
        COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/unit/xir/integer_bits_resources.py
            $<TARGET_FILE:test_xir_stdlib_resources> ${PROJECT_SOURCE_DIR} ${bits_binding_mode})
    add_test(NAME test_xir_integer_bits_runtime_${mode}
        COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/unit/xir/integer_bits_resources.py
            $<TARGET_FILE:test_xir_integer_bits_vm> ${PROJECT_SOURCE_DIR} ${mode})
    set_tests_properties(test_xir_integer_bits_preparation_${mode} test_xir_integer_bits_binding_${mode} test_xir_integer_bits_runtime_${mode} PROPERTIES
        TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1 LABELS "unit;xir;ownership;stdlib;resources;integer")
endforeach()

foreach(mode IN ITEMS measure faults)
    add_test(NAME test_xir_integer_preparation_${mode}
        COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/unit/xir/integer_methods_resources.py
            $<TARGET_FILE:test_xir_stdlib_preparation> ${PROJECT_SOURCE_DIR} ${mode})
    set_tests_properties(test_xir_integer_preparation_${mode} PROPERTIES
        TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1 LABELS "unit;xir;ownership;stdlib;resources;integer")
    if(ENABLE_ASAN AND mode STREQUAL "faults")
        set_tests_properties(test_xir_integer_preparation_${mode} PROPERTIES COST 300)
    endif()
endforeach()

foreach(mode IN ITEMS measure faults)
    if(mode STREQUAL "measure")
        set(integer_binding_mode binding-measure)
    else()
        set(integer_binding_mode faults)
    endif()
    add_test(NAME test_xir_integer_binding_${mode}
        COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/unit/xir/integer_methods_resources.py
            $<TARGET_FILE:test_xir_stdlib_resources> ${PROJECT_SOURCE_DIR} ${integer_binding_mode})
    set_tests_properties(test_xir_integer_binding_${mode} PROPERTIES
        TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1 LABELS "unit;xir;ownership;stdlib;resources;integer")
    if(ENABLE_ASAN AND mode STREQUAL "faults")
        set_tests_properties(test_xir_integer_binding_${mode} PROPERTIES COST 200)
    endif()
endforeach()

add_executable(test_xir_integer_methods_vm ${CMAKE_CURRENT_LIST_DIR}/test_xir_integer_methods_vm.c)
target_include_directories(test_xir_integer_methods_vm PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(test_xir_integer_methods_vm PRIVATE xray_xir_stdlib_catalog xray_xir_vm)
if(MSVC)
    target_compile_options(test_xir_integer_methods_vm PRIVATE /W4 /WX)
else()
    target_compile_options(test_xir_integer_methods_vm PRIVATE -Wall -Wextra -Werror)
endif()
foreach(mode IN ITEMS measure faults)
    add_test(NAME test_xir_integer_runtime_${mode}
        COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/unit/xir/integer_methods_resources.py
            $<TARGET_FILE:test_xir_integer_methods_vm> ${PROJECT_SOURCE_DIR} ${mode})
    set_tests_properties(test_xir_integer_runtime_${mode} PROPERTIES
        TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1 LABELS "unit;xir;ownership;stdlib;resources;integer")
endforeach()
