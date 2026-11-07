# One whole Source Program retains all twenty original Atomic native expectations.
set(XIR_ATOMIC_NATIVE20 ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_atomic_native20.c)
set(XIR_ATOMIC_FIXTURE ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_atomic_native20)

set(atomic_native_names atomic_i64 atomic_bool atomic_i64_cas_1 atomic_i64_cas_2 atomic_i64_cas_3 atomic_i64_cas_4
    atomic_bool_cas_1 atomic_bool_cas_2 atomic_bool_cas_3 atomic_bool_cas_4 atomic_i64_update_5 atomic_i64_update_6
    atomic_i64_update_7 atomic_i64_update_8 atomic_i64_update_9 atomic_i64_update_10 atomic_bool_update_7 atomic_bool_update_10
    atomic_i64_allocations atomic_bool_allocations)
set(atomic_native_exits 42 1 42 40 40 40 1 0 0 0 42 38 42 40 40 40 1 0 0 0)
set(atomic_case_index 0)
set(atomic_all_targets)
foreach(atomic_name IN LISTS atomic_native_names)
    set(atomic_target test_xr_program_aot_${atomic_name})

    list(GET atomic_native_exits ${atomic_case_index} atomic_exit)

    list(APPEND atomic_all_targets)
    math(EXPR atomic_case_index "${atomic_case_index}+1")
endforeach()
