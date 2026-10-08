# Retiring consumers retain exact source inputs and independent result goldens.
set(product_consumer_source "${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/test_source_product_consumer.c")
set(product_consumer_fixture_root "${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/fixtures")
get_filename_component(product_consumer_fixture_root "${product_consumer_fixture_root}" REALPATH)
set(product_consumer_fault_runner "${CMAKE_SOURCE_DIR}/scripts/source_product_consumer_faults.py")
set(product_consumer_main_template "${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_consumer_main.c.in")
add_library(source_product_consumer_driver OBJECT "${product_consumer_source}")
target_link_libraries(source_product_consumer_driver PUBLIC xray_xir_source_product xray_xir_runtime_host)
target_compile_definitions(source_product_consumer_driver PRIVATE XR_CONSUMER_STDLIB="${CMAKE_SOURCE_DIR}/stdlib")
target_include_directories(source_product_consumer_driver PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers")
set_target_properties(source_product_consumer_driver PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(source_product_consumer_driver PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(source_product_consumer_driver PRIVATE -Wall -Wextra -Werror)
endif()
add_executable(test_source_product_probe "${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_consumer_probe.c")
target_link_libraries(test_source_product_probe PRIVATE source_product_consumer_driver)
set_target_properties(test_source_product_probe PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_product_probe PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_source_product_probe PRIVATE -Wall -Wextra -Werror)
endif()
include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_consumer_cases.cmake")
foreach(case IN LISTS product_consumer_cases)
    set(expected_variable "product_consumer_expected_${case}")
    set(expected "${${expected_variable}}")
    set(producer "test_source_product_${case}")
    set(native "${producer}_native")
    set(generated "${CMAKE_BINARY_DIR}/generated/source_product_${case}.c")
    set(consumer_has_native 0)
    set(main "${CMAKE_CURRENT_BINARY_DIR}/source_product_${case}_main.c")
    configure_file("${product_consumer_main_template}" "${main}" @ONLY)
    add_executable(${producer} "${main}")
    target_link_libraries(${producer} PRIVATE source_product_consumer_driver)
    file(GLOB product_consumer_case_sources "${product_consumer_fixture_root}/${case}/*.xr")
    if(case STREQUAL "time_sleep")
        list(APPEND product_consumer_case_sources "${CMAKE_SOURCE_DIR}/stdlib/time/time.xr")
    endif()
    add_custom_command(OUTPUT "${generated}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated"
        COMMAND $<TARGET_FILE:${producer}> 0 "${generated}"
        DEPENDS ${producer} ${product_consumer_case_sources}
        VERBATIM)
    set(consumer_has_native 1)
    set(native_main "${CMAKE_CURRENT_BINARY_DIR}/source_product_${case}_native_main.c")
    configure_file("${product_consumer_main_template}" "${native_main}" @ONLY)
    add_executable(${native} "${native_main}" "${generated}")
    target_link_libraries(${native} PRIVATE source_product_consumer_driver)
    foreach(target IN ITEMS ${producer} ${native})
        set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
        if(MSVC)
            target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
        else()
            target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
        endif()
    endforeach()
    add_test(NAME ${producer}_vm COMMAND ${producer} 0)
    add_test(NAME ${producer}_native COMMAND ${native} 1)
    add_test(NAME ${producer}_mixed_even COMMAND ${native} 2)
    add_test(NAME ${producer}_mixed_odd COMMAND ${native} 3)
    set_tests_properties(${producer}_vm ${producer}_native ${producer}_mixed_even
        ${producer}_mixed_odd PROPERTIES TIMEOUT 120
        LABELS "unit;xir;source-product;program-consumer;ownership;execution")
    foreach(mode IN ITEMS 0 1 2 3)
        if(mode EQUAL 0)
            set(executable ${producer})
        else()
            set(executable ${native})
        endif()
        set(consumer_checks axes runtime cancel)
        foreach(check IN LISTS consumer_checks)
            add_test(NAME ${producer}_${check}_${mode} COMMAND ${executable} ${mode} --${check})
            set_tests_properties(${producer}_${check}_${mode} PROPERTIES TIMEOUT 120
                LABELS "unit;xir;source-product;program-consumer;ownership;${check}")
        endforeach()
        add_test(NAME ${producer}_compiler_${mode}
            COMMAND ${XRAY_PYTHON} -X utf8 "${product_consumer_fault_runner}"
                --binary $<TARGET_FILE:${executable}> --mode ${mode} --jobs 8
                --evidence "${CMAKE_BINARY_DIR}/consumer-fault-evidence/${case}-${mode}")
        set_tests_properties(${producer}_compiler_${mode} PROPERTIES TIMEOUT 600 PROCESSORS 8
            LABELS "unit;xir;source-product;program-consumer;ownership;compiler-faults")
        if(case STREQUAL "text_program" OR case STREQUAL "canonical_initializer")
            add_test(NAME ${producer}_output_status_${mode} COMMAND ${executable} ${mode} --output-status)
            set_tests_properties(${producer}_output_status_${mode} PROPERTIES TIMEOUT 120
                LABELS "unit;xir;source-product;program-consumer;ownership;output")
        endif()
    endforeach()
endforeach()
include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_nominal_graph_cases.cmake")
add_library(source_product_nominal_driver OBJECT "${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/test_source_product_nominal_graphs.c")
target_link_libraries(source_product_nominal_driver PUBLIC xray_xir_source_product)
target_include_directories(source_product_nominal_driver PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers")
set_target_properties(source_product_nominal_driver PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(source_product_nominal_driver PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(source_product_nominal_driver PRIVATE -Wall -Wextra -Werror)
endif()
foreach(case IN LISTS product_nominal_graph_cases)
    set(expected_variable "product_nominal_graph_expected_${case}")
    set(expected "${${expected_variable}}")
    set(producer "test_source_product_${case}")
    set(main "${CMAKE_CURRENT_BINARY_DIR}/source_product_${case}_main.c")
    configure_file("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_nominal_graph_main.c.in" "${main}" @ONLY)
    add_executable(${producer} "${main}")
    target_link_libraries(${producer} PRIVATE source_product_nominal_driver)
    set(generated "${CMAKE_BINARY_DIR}/generated/source_product_${case}.c")
    add_custom_command(OUTPUT "${generated}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated"
        COMMAND $<TARGET_FILE:${producer}> 0 "${generated}"
        DEPENDS ${producer} "${product_consumer_fixture_root}/${case}/root.xr" VERBATIM)
    add_library(${producer}_native_object OBJECT "${generated}")
    target_link_libraries(${producer}_native_object PRIVATE xray_xir_source_product)
    foreach(target IN ITEMS ${producer} ${producer}_native_object)
        set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
        if(MSVC)
            target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
        else()
            target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
        endif()
    endforeach()
    add_test(NAME ${producer} COMMAND ${producer} 0)
    add_test(NAME ${producer}_axes COMMAND ${producer} 0 --axes)
    set_tests_properties(${producer} ${producer}_axes PROPERTIES TIMEOUT 120
        LABELS "unit;xir;source-product;program-consumer;ownership;nominal-graph")
    add_test(NAME ${producer}_compiler COMMAND ${XRAY_PYTHON} -X utf8 "${product_consumer_fault_runner}"
        --binary $<TARGET_FILE:${producer}> --mode 0 --jobs 8
        --evidence "${CMAKE_BINARY_DIR}/consumer-fault-evidence/${case}-0")
    set_tests_properties(${producer}_compiler PROPERTIES TIMEOUT 600 PROCESSORS 8
        LABELS "unit;xir;source-product;program-consumer;ownership;compiler-faults")
endforeach()
add_executable(test_source_product_detachment
    "${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/test_source_product_detachment.c")
target_link_libraries(test_source_product_detachment PRIVATE xray_xir_source_product)
target_compile_definitions(test_source_product_detachment PRIVATE
    XR_DETACHMENT_ROOT="${product_consumer_fixture_root}"
    XR_DETACHMENT_SCRATCH="${CMAKE_BINARY_DIR}/consumer-detachment-inputs")
set_target_properties(test_source_product_detachment PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_product_detachment PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_source_product_detachment PRIVATE -Wall -Wextra -Werror)
endif()
foreach(case IN ITEMS generics callables single_module text_program cross_module_coroutine cross_module_static_coroutine)
    add_test(NAME test_source_product_detachment_${case}
        COMMAND ${XRAY_PYTHON} -X utf8 "${CMAKE_SOURCE_DIR}/scripts/source_product_consumer_detachment.py"
            --binary $<TARGET_FILE:test_source_product_detachment> --case ${case}
            --scratch "${CMAKE_BINARY_DIR}/consumer-detachment-inputs")
    set_tests_properties(test_source_product_detachment_${case} PROPERTIES TIMEOUT 120
        LABELS "unit;xir;source-product;program-consumer;ownership;detachment")
    add_test(NAME test_source_product_detachment_${case}_axes
        COMMAND ${XRAY_PYTHON} -X utf8 "${CMAKE_SOURCE_DIR}/scripts/source_product_consumer_detachment.py"
            --binary $<TARGET_FILE:test_source_product_detachment> --case ${case}
            --scratch "${CMAKE_BINARY_DIR}/consumer-detachment-inputs" --check axes)
    set_tests_properties(test_source_product_detachment_${case}_axes PROPERTIES TIMEOUT 120
        LABELS "unit;xir;source-product;program-consumer;ownership;detachment;axes")
    add_test(NAME test_source_product_detachment_${case}_compiler
        COMMAND ${XRAY_PYTHON} -X utf8 "${CMAKE_SOURCE_DIR}/scripts/source_product_consumer_detachment_faults.py"
            --binary $<TARGET_FILE:test_source_product_detachment> --case ${case}
            --scratch "${CMAKE_BINARY_DIR}/consumer-detachment-inputs" --jobs 8
            --evidence "${CMAKE_BINARY_DIR}/consumer-detachment-fault-evidence/${case}")
    set_tests_properties(test_source_product_detachment_${case}_compiler PROPERTIES TIMEOUT 600 PROCESSORS 8
        LABELS "unit;xir;source-product;program-consumer;ownership;detachment;compiler-faults")
endforeach()
add_executable(test_source_product_rejections
    "${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/test_source_product_rejections.c")
target_link_libraries(test_source_product_rejections PRIVATE xray_xir_source_product)
target_compile_definitions(test_source_product_rejections PRIVATE XR_REJECTION_ROOT="${product_consumer_fixture_root}")
set_target_properties(test_source_product_rejections PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_product_rejections PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_source_product_rejections PRIVATE -Wall -Wextra -Werror)
endif()
foreach(case IN ITEMS bare_nullable missing_name)
    set(test "test_source_product_rejections_${case}")
    add_test(NAME ${test} COMMAND test_source_product_rejections ${case})
    add_test(NAME ${test}_axes COMMAND test_source_product_rejections ${case} --axes)
    add_test(NAME ${test}_compiler COMMAND test_source_product_rejections ${case} --compiler)
    set_tests_properties(${test} ${test}_axes PROPERTIES TIMEOUT 120
        LABELS "unit;xir;source-product;program-consumer;ownership;rejection")
    set_tests_properties(${test}_compiler PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE
        LABELS "unit;xir;source-product;program-consumer;ownership;compiler-faults")
endforeach()
add_test(NAME source_product_consumer_inventory
    COMMAND ${XRAY_PYTHON} -X utf8 "${CMAKE_SOURCE_DIR}/scripts/source_product_consumer_inventory.py" check)
set_tests_properties(source_product_consumer_inventory PROPERTIES TIMEOUT 30
    LABELS "unit;xir;source-product;program-consumer;inventory")
add_executable(test_source_product_integer_constructors
    "${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_integer_constructors.c")
set_target_properties(test_source_product_integer_constructors PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_product_integer_constructors PRIVATE /utf-8 /W4 /WX)
    target_compile_definitions(test_source_product_integer_constructors PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(test_source_product_integer_constructors PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME source_product_integer_source_constructors
    COMMAND ${XRAY_PYTHON} -X utf8 "${CMAKE_SOURCE_DIR}/scripts/source_product_consumer_numeric_sources.py"
        --binary $<TARGET_FILE:test_source_product_integer_constructors>
        --scratch "${CMAKE_BINARY_DIR}/consumer-integer-constructor-evidence")
set_tests_properties(source_product_integer_source_constructors PROPERTIES TIMEOUT 120
    LABELS "unit;xir;source-product;program-consumer;inventory;integer")
add_executable(test_source_product_nominal_constructors
    "${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_nominal_constructors.c")
target_link_libraries(test_source_product_nominal_constructors PRIVATE xray_xir_source_product)
set_target_properties(test_source_product_nominal_constructors PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_product_nominal_constructors PRIVATE /utf-8 /W4 /WX)
    target_compile_definitions(test_source_product_nominal_constructors PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(test_source_product_nominal_constructors PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME source_product_nominal_source_constructors
    COMMAND ${XRAY_PYTHON} -X utf8 "${CMAKE_SOURCE_DIR}/scripts/source_product_consumer_nominal_sources.py"
        --binary $<TARGET_FILE:test_source_product_nominal_constructors>
        --scratch "${CMAKE_BINARY_DIR}/consumer-nominal-constructor-evidence")
set_tests_properties(source_product_nominal_source_constructors PROPERTIES TIMEOUT 120
    LABELS "unit;xir;source-product;program-consumer;inventory;nominal-graph")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_packet_roles.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_entry_roles.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_semantic67_packets.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_semantic67_writer.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_timer_effects.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_timer_cgen.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_semantic67_writer_stage_normal.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_semantic67_writer_budget_occupied.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_semantic67_writer_prefix_fi.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_allocation_scenario0.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_allocation_original.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_atomic_source_families.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_atomic_ordering_oracles.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_allocation_scenario0_native.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_atomic_i64_cas_oracles.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_atomic_i64_cas_native.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_atomic_source_families_native.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_assertion_message.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_integer_division_defer.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_builtin_panic_group6.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_call_error_panic_group5.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_coroutine_panic_group4.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_default_assertion_group2.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_imported_enum_catch.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_imported_enum_catch_native.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_assertion_message_native.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_default_assertion_group2_native.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_integer_intrinsic_panic_group3_native.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_call_panic_group2_native.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_panic_only_defer_group2_native.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_recursive_call_panic_native.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_typed_invoke_group4_native.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_coroutine_panic_group2_native.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_coroutine_invoke_group2_native.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_source_file_deletion_native.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_source_file_deletion_vm.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_request_budget_inputs.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_module_initializer_admission.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_checked_hostile_frame.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_f64_transport.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_source_f64_transport.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_nominal_names.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_module_graph.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_module_execution.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_module_output_failure.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_module_suspension.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_module_cancel_cleanup.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_module_slots.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_module_slot_state.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_module_slot_failure.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_module_class_state.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_module_class_suspend.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_module_class_error.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_module_class_panic.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_module_operation.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_module_place.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_module_ref_source.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_array_types.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_atomic_types.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_parent_source.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_channel_owner.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_process_lifecycle.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_integer_conversions.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_integer_divmod_admission.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_integer_bitwise_admission.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_integer_width.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_rejections_occupied.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_generic_value_struct.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_remaining_source.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_array_append_admission.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_array_append_source.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_array_append_managed.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_array_default_admission.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_array_default_source.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_array_source_refusals.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_array_default_escape.cmake")
