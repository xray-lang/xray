add_executable(test_xir_p11_ctfe_reference "${CMAKE_CURRENT_LIST_DIR}/test_ctfe_reference.c")
target_link_libraries(test_xir_p11_ctfe_reference PRIVATE xray_xir_source xray_xir_vm)
target_compile_definitions(test_xir_p11_ctfe_reference PRIVATE
    XR_CTFE_REFERENCE_FIXTURES="${CMAKE_CURRENT_LIST_DIR}/fixtures")
set_target_properties(test_xir_p11_ctfe_reference PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_p11_ctfe_reference PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_xir_p11_ctfe_reference PRIVATE -Wall -Wextra -Werror)
endif()
foreach(name IN ITEMS struct_identity_array struct_identity_nested struct_identity_nullable struct_identity_capture struct_identity_error struct_identity_cow struct_identity_alias struct_identity_acyclic_replace struct_identity_function_cycle struct_identity_class_cycle struct_identity_export struct_identity_projected_root_refused struct_identity_absent_loan struct_identity_active_loan struct_identity_direct struct_identity_owning_direct struct_identity_owning struct_identity_class struct_identity_function ref_closure_result ref_closure_unit ref_closure_return ref_closure_explicit ref_closure_context ref_closure_nested ref_closure_arrow ref_closure_readonly ref_closure_reborrow ref_closure_capture ref_closure_cell_capture ref_closure_array ref_closure_projection ref_closure_cleanup ref_closure_generic_context ref_closure_panic ref_closure_alias ref_closure_direct ref_closure_owning_direct ref_closure_owning array_identity_projected_root_refused array_identity_direct array_identity_owning_direct array_identity_owning array_identity_cow array_identity_repeat array_identity_function_set_cycle array_identity_function_path_cycle array_identity_error array_identity_empty_ref array_identity_function_cycle array_identity_basic array_identity_alias array_identity_mutate array_identity_nested array_identity_nullable array_identity_enum array_identity_tuple array_identity_function array_identity_field array_identity_cycle array_identity_export class_inactive_direct class_inactive_owning_direct class_inactive_owning class_inactive_nested class_inactive_enum class_inactive_tuple class_inactive_dynamic class_inactive_reborrow class_inactive_active_child class_inactive_identity_leaf class_ref_dynamic_fields class_ref_dynamic_direct class_ref_dynamic_owning_direct class_ref_dynamic_reborrow class_ref_dynamic_replace class_ref_dynamic_cleanup class_ref_dynamic_error class_ref_dynamic_panic class_ref_dynamic_cow class_ref_dynamic_nullable class_ref_dynamic_enum class_ref_dynamic_owning output_heap_limit output_fibonacci output_fibonacci_direct output_pair output_pair_direct class_authority_direct class_work_direct class_tuple class_nullable class_enum class_capture class_cell class_chain class_cycle class_self_cycle class_error class_panic class_ref_reborrow class_ref_grow class_ref_dynamic_gap class_deep_ref_gap class_return_string class_owning_direct class_export class_cycle_direct class_acyclic_replace class_field_struct class_empty class_ctor class_write class_alias class_method class_array class_direct panic_projection_recheck panic_abort_direct panic_escape_direct panic_owning_direct panic_work_direct panic_nested panic_error_channel panic_invoke panic_pending_cleanup panic_cleanup_local panic_error_cleanup_local panic_tuple panic_nullable panic_array panic_struct panic_enum panic_capture panic_cell panic_null panic_range panic_assert panic_assert_true panic_assert_panics panic_assert_normal panic_project_bounds panic_path_bounds panic_eval_once panic_return_info panic_cleanup_escape panic_io panic_unhandled_assert panic_owning panic_local panic_code panic_cleanup panic_cross panic_bounds_message panic_direct error_nullable error_cycle_direct error_cycle_acyclic error_cleanup_invoke error_caller_cleanup error_nested_holder error_work_direct error_higher error_generic error_holder error_cell error_abort_direct error_success error_unit error_discard error_rethrow error_filter error_cleanup_nested error_cleanup_catch error_snapshot error_unhandled error_io error_infinite_cleanup error_recursion error_owning_direct error_local error_call error_cleanup error_ref error_direct project_owning_direct project_reproject project_suffix project_reserve project_evaluate_once project_bounds project_io project_field project_mixed project_cow project_tuple project_higher project_direct project_index project_nested project_reborrow project_grow project_cleanup cleanup_infinite cleanup_generic cleanup_owning_direct ref_alias_source cleanup_empty cleanup_reborrow cleanup_return_string cleanup_path ref_higher ref_array cleanup_direct cleanup_io cleanup_scope cleanup_lifo cleanup_loop cleanup_return cleanup_owned cleanup_nested cleanup_function ref_simple ref_nested function_cycle_direct function_cell_replace function_cell_acyclic function_tuple function_nullable function_array_methods function_direct function_phi function_recursion function_capture_array function_capture_string function_capture_function function_return_array function_float function_cycle function_io function_export function_ref function_lambda function_capture_copy function_capture_cell function_return function_higher lexical_private_direct lexical_nested lexical_class_instance lexical_class_io lexical_oversize lexical_struct_construct lexical_struct_export lexical_private_call lexical_class_static lexical_io lexical_unauthorized nominal_work nominal_private nominal_enum_array_export nominal_struct_export nominal_struct_empty_export nominal_enum_export nominal_enum_empty_export nominal_enum_single_export nominal_nested_export nominal_struct_direct nominal_enum_direct nominal_field_array nominal_large nominal_dynamic_gap nominal_class nominal_struct nominal_struct_method nominal_struct_generic nominal_struct_array nominal_struct_cow nominal_enum nominal_enum_empty nominal_nested nominal_io text_bool_false text_widths text_work text_cell text_holder text_integer text_unsigned text_bool text_rune text_float text_float_extreme text_direct text_io text_oversize array_methods_fill_range array_methods_fill_invalid array_methods_empty array_path_work array_methods_query array_methods_join array_methods_reorder array_methods_extract array_methods_clear array_methods_fill array_methods_resize array_methods_concat array_path_string_direct array_path_set array_path_push array_path_reserve array_path_cow array_path_bounds array_path_io array_path_export array_path_direct equal_array_work equal_array_int equal_array_float equal_array_string equal_array_nested equal_array_nullable equal_nullable_array equal_array_direct array_holder array_deep_gap array_repeat array_reserve array_export array_empty array_nested array_string array_tuple array_nullable array_capacity array_cell array_internal array_bounds array_io nullable_units nullable_phi nullable_tuple_field nullable_string_equal nullable_work nullable_some nullable_none nullable_nested nullable_some_none nullable_string nullable_tuple nullable_cell nullable_internal nullable_equal nullable_unwrap nullable_oversize nullable_io nullable_direct float_tuple float_f64 float_f32 float_ieee float_convert float_round float_cell float_zero float_range float_io float_direct tuple_generic tuple_export tuple_nested tuple_units tuple_cell tuple_internal tuple_phi tuple_oversize tuple_io string_order string_methods string_search string_search_empty string_read_direct string_bounds_negative string_bounds_end string_read_io string_export string_empty string_nul string_cell string_predicate string_overflow cell_rune cell_loop cell_recursion cell_unit cell_infinite cell_io cell_leaf rune_literal rune_nested rune_equal rune_direct rune_surrogate rune_overflow factorial unit_empty unit_statements unit_direct unit_io unit_infinite unit_closure unit_recipe unit_nested short_circuit logical_truth_table logical_skip_division logical_recursion leaf_locals mutual_parity frame_state unreachable_function_io reachable_short_circuit_io reachable_branch_io mutual_depth mutual_depth_direct)
    add_test(NAME test_xir_p11_ctfe_reference_${name} COMMAND test_xir_p11_ctfe_reference ${name})
    set_tests_properties(test_xir_p11_ctfe_reference_${name} PROPERTIES
        TIMEOUT 30 LABELS "unit;xir;ctfe;reference;ownership")
endforeach()
xray_stage_windows_asan_runtime("${CMAKE_CURRENT_BINARY_DIR}")
xray_prepend_windows_asan_runtime_to_directory_tests()

# Complete evaluator fault jobs measured near the original30-second ASan cap.
# Serialize these real receiving/verification jobs without changing any limit.
set_tests_properties(test_xir_p11_ctfe_reference_error_direct
    test_xir_p11_ctfe_reference_error_owning_direct
    test_xir_p11_ctfe_reference_error_abort_direct PROPERTIES RUN_SERIAL TRUE)

set_tests_properties(test_xir_p11_ctfe_reference_panic_direct
    test_xir_p11_ctfe_reference_panic_owning_direct PROPERTIES RUN_SERIAL TRUE)

set_tests_properties(test_xir_p11_ctfe_reference_panic_abort_direct
    test_xir_p11_ctfe_reference_panic_escape_direct PROPERTIES RUN_SERIAL TRUE)

set_tests_properties(test_xir_p11_ctfe_reference_class_direct
    test_xir_p11_ctfe_reference_class_owning_direct
    test_xir_p11_ctfe_reference_class_cycle_direct PROPERTIES RUN_SERIAL TRUE)

set_tests_properties(test_xir_p11_ctfe_reference_output_fibonacci_direct
    test_xir_p11_ctfe_reference_output_pair_direct PROPERTIES RUN_SERIAL TRUE)

# Measured sanitizer cost puts the largest serial fault job early on cold schedulers.
set_tests_properties(test_xir_p11_ctfe_reference_class_direct PROPERTIES COST 29)

set_tests_properties(test_xir_p11_ctfe_reference_class_ref_dynamic_direct
    test_xir_p11_ctfe_reference_class_ref_dynamic_owning_direct PROPERTIES RUN_SERIAL TRUE)

# Complete fault jobs retain their measured serial sanitizer scheduling costs.
set_tests_properties(test_xir_p11_ctfe_reference_class_ref_dynamic_owning PROPERTIES
    RUN_SERIAL TRUE COST 24)
set_tests_properties(test_xir_p11_ctfe_reference_class_ref_dynamic_direct PROPERTIES COST 26)
set_tests_properties(test_xir_p11_ctfe_reference_class_ref_dynamic_owning_direct PROPERTIES COST 27)

set_tests_properties(test_xir_p11_ctfe_reference_class_inactive_direct
    test_xir_p11_ctfe_reference_class_inactive_owning_direct
    test_xir_p11_ctfe_reference_class_inactive_owning PROPERTIES RUN_SERIAL TRUE)

set_tests_properties(test_xir_p11_ctfe_reference_array_identity_direct
    test_xir_p11_ctfe_reference_array_identity_owning_direct
    test_xir_p11_ctfe_reference_array_identity_owning PROPERTIES RUN_SERIAL TRUE)

set_tests_properties(test_xir_p11_ctfe_reference_ref_closure_direct
    test_xir_p11_ctfe_reference_ref_closure_owning_direct
    test_xir_p11_ctfe_reference_ref_closure_owning PROPERTIES RUN_SERIAL TRUE)

set_tests_properties(test_xir_p11_ctfe_reference_struct_identity_direct
    test_xir_p11_ctfe_reference_struct_identity_owning_direct
    test_xir_p11_ctfe_reference_struct_identity_owning PROPERTIES RUN_SERIAL TRUE)

add_executable(test_xir_ctfe_slice_u8 "${CMAKE_CURRENT_LIST_DIR}/test_ctfe_slice_u8.c")
target_link_libraries(test_xir_ctfe_slice_u8 PRIVATE xray_xir_source xray_xir_vm)
target_compile_definitions(test_xir_ctfe_slice_u8 PRIVATE XR_CTFE_SLICE_FIXTURES="${CMAKE_CURRENT_LIST_DIR}/fixtures")
set_target_properties(test_xir_ctfe_slice_u8 PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_ctfe_slice_u8 PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_xir_ctfe_slice_u8 PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
foreach(name IN ITEMS slice_u8_local slice_u8_string slice_u8_arguments)
    add_test(NAME test_xir_ctfe_${name} COMMAND test_xir_ctfe_slice_u8 ${name})
    set_tests_properties(test_xir_ctfe_${name} PROPERTIES LABELS "unit;xir;ctfe;ownership" TIMEOUT 60)
endforeach()

foreach(mode IN ITEMS resources authority controls boundaries memory runtime-work argument-resources argument-authority argument-cancel argument-memory)
    add_test(NAME test_xir_ctfe_slice_u8_${mode} COMMAND test_xir_ctfe_slice_u8 ${mode})
    set_tests_properties(test_xir_ctfe_slice_u8_${mode} PROPERTIES LABELS "unit;xir;ctfe;ownership;resources" TIMEOUT 60)
endforeach()

set(ctfe_from_utf8_fixture "${CMAKE_CURRENT_LIST_DIR}/fixtures/from_utf8.xr")
set(ctfe_from_utf8_stdout "${CMAKE_CURRENT_LIST_DIR}/fixtures/from_utf8.stdout")
set(ctfe_from_utf8_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/ctfe_from_utf8.c")
add_custom_command(OUTPUT "${ctfe_from_utf8_generated}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_from_utf8> --emit-file "${ctfe_from_utf8_fixture}" "${ctfe_from_utf8_generated}"
    DEPENDS test_xir_from_utf8 "${ctfe_from_utf8_fixture}" VERBATIM)
add_executable(test_xir_ctfe_from_utf8_native
    "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_from_utf8_native.c" "${ctfe_from_utf8_generated}")
target_link_libraries(test_xir_ctfe_from_utf8_native PRIVATE xray_xir_scalar xray_compile_resources)
set_target_properties(test_xir_ctfe_from_utf8_native PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_ctfe_from_utf8_native PRIVATE /utf-8 /W4 /WX)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_xir_ctfe_from_utf8_native PRIVATE /experimental:c11atomics)
    endif()
else()
    target_compile_options(test_xir_ctfe_from_utf8_native PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME test_xir_ctfe_from_utf8
    COMMAND test_xir_from_utf8 --vm-file "${ctfe_from_utf8_fixture}" --expect "${ctfe_from_utf8_stdout}")
add_test(NAME test_xir_ctfe_from_utf8_native
    COMMAND test_xir_ctfe_from_utf8_native --expect "${ctfe_from_utf8_stdout}")
set_tests_properties(test_xir_ctfe_from_utf8 test_xir_ctfe_from_utf8_native PROPERTIES
    LABELS "unit;xir;ctfe;source;ownership;fast" TIMEOUT 60)

target_compile_definitions(test_xir_ctfe_slice_u8 PRIVATE XR_CTFE_UTF8_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
foreach(mode oom control lifetime types memory packet)
    add_test(NAME test_xir_ctfe_from_utf8_${mode} COMMAND test_xir_ctfe_slice_u8 native-${mode})
    set_tests_properties(test_xir_ctfe_from_utf8_${mode} PROPERTIES LABELS "unit;xir;ctfe;ownership;resources" TIMEOUT 60)
endforeach()

set(try_assert_fixture "${CMAKE_CURRENT_LIST_DIR}/fixtures/try_assert.xr")
set(try_assert_stdout "${CMAKE_CURRENT_LIST_DIR}/fixtures/try_assert.stdout")
set(try_assert_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/try_assert.c")
add_custom_command(OUTPUT "${try_assert_generated}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_from_utf8> --emit-file "${try_assert_fixture}" "${try_assert_generated}"
    DEPENDS test_xir_from_utf8 "${try_assert_fixture}" VERBATIM)
add_executable(test_xir_try_assert_native
    "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_from_utf8_native.c" "${try_assert_generated}")
target_link_libraries(test_xir_try_assert_native PRIVATE xray_xir_scalar xray_compile_resources)
set_target_properties(test_xir_try_assert_native PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_try_assert_native PRIVATE /utf-8 /W4 /WX)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_xir_try_assert_native PRIVATE /experimental:c11atomics)
    endif()
else()
    target_compile_options(test_xir_try_assert_native PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME test_xir_try_assert_source
    COMMAND test_xir_from_utf8 --vm-file "${try_assert_fixture}" --expect "${try_assert_stdout}")
add_test(NAME test_xir_try_assert_native
    COMMAND test_xir_try_assert_native --expect "${try_assert_stdout}")
set_tests_properties(test_xir_try_assert_source test_xir_try_assert_native PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;panic" TIMEOUT 60)

foreach(mode owner vm resources controls memory rejections)
    add_test(NAME test_xir_try_assert_${mode} COMMAND test_xir_ctfe_slice_u8 try-${mode})
    set_tests_properties(test_xir_try_assert_${mode} PROPERTIES LABELS "unit;xir;ctfe;ownership;panic;resources" TIMEOUT 60)
endforeach()

foreach(case task_unsafe task_safe ordinary_unsafe order)
    set(fixture "${CMAKE_CURRENT_LIST_DIR}/fixtures/try_assert_${case}.xr")
    set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated/try_assert_${case}.c")
    set(target "test_xir_try_assert_${case}_native")
    add_custom_command(OUTPUT "${generated}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
        COMMAND $<TARGET_FILE:test_xir_from_utf8> --emit-file "${fixture}" "${generated}"
        DEPENDS test_xir_from_utf8 "${fixture}" VERBATIM)
    add_executable(${target} "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_from_utf8_native.c" "${generated}")
    target_link_libraries(${target} PRIVATE xray_xir_scalar xray_compile_resources)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
        if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
            target_compile_options(${target} PRIVATE /experimental:c11atomics)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -pedantic)
    endif()
    if(case STREQUAL "task_unsafe")
        add_test(NAME test_xir_try_assert_${case}_source COMMAND test_xir_from_utf8 --vm-file "${fixture}" --expect-bad-state)
        add_test(NAME ${target} COMMAND ${target} --expect-bad-state)
    else()
        set(oracle "${CMAKE_CURRENT_LIST_DIR}/fixtures/try_assert_${case}.stdout")
        add_test(NAME test_xir_try_assert_${case}_source COMMAND test_xir_from_utf8 --vm-file "${fixture}" --expect "${oracle}")
        add_test(NAME ${target} COMMAND ${target} --expect "${oracle}")
    endif()
    set_tests_properties(test_xir_try_assert_${case}_source ${target} PROPERTIES LABELS "unit;xir;panic;ownership;task" TIMEOUT 120)
endforeach()

add_test(NAME test_xir_try_assert_graph_source COMMAND test_xir_from_utf8 --try-graph)
set_tests_properties(test_xir_try_assert_graph_source PROPERTIES LABELS "unit;xir;panic;ownership" TIMEOUT 120)

add_test(NAME test_xir_try_assert_lexical_source COMMAND test_xir_from_utf8 --try-rejections)
add_test(NAME test_xir_try_assert_never_source COMMAND test_xir_from_utf8 --vm-file "${CMAKE_CURRENT_LIST_DIR}/fixtures/try_assert_never.xr" --expect "${CMAKE_CURRENT_LIST_DIR}/fixtures/try_assert_never.stdout")
set_tests_properties(test_xir_try_assert_lexical_source test_xir_try_assert_never_source PROPERTIES LABELS "unit;xir;panic;errors" TIMEOUT 120)
