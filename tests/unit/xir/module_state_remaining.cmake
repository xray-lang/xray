# Four-slot class initialization preserves actual identity and allocator releases.

set(state_remaining_targets test_xir_module_state_remaining)
foreach(state_remaining_scenario IN ITEMS 6 7 9 10 11 12 13)
    set(state_remaining_c ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_module_state_remaining_${state_remaining_scenario}.c)

    set(state_remaining_target test_xr_program_aot_module_state_${state_remaining_scenario})

    list(APPEND state_remaining_targets)
endforeach()
