# One real Source product replaces only the original fifteen Array generators.
set(XIR_ARRAY_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_array_generated.c)
set(XIR_ARRAY_FIXTURE ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_array_generated)

set(array_native_names array_alias array_strings array_empty array_integers array_nested
    array_allocations_0 array_allocations_1 array_allocations_2 array_allocations_3 array_allocations_4
    array_default array_default_allocations array_append array_append_allocations array_append_managed_allocations)
set(array_native_exits 0 2 0 2 1 0 0 0 0 0 0 0 0 0 0)
set(array_case_index 0)
foreach(array_case_name IN LISTS array_native_names)
    set(array_target test_xr_program_aot_${array_case_name})

    list(GET array_native_exits ${array_case_index} array_expected_exit)

    list(APPEND array_all_targets)
    math(EXPR array_case_index "${array_case_index}+1")
endforeach()
list(APPEND array_all_targets)
