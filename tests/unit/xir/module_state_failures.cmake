# Complete Built programs retain each initialization failure as a sticky outcome.

set(state_failure_targets test_xir_module_state_failures)
foreach(state_failure_scenario RANGE 2 4)
    set(state_failure_c ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_module_state_failure_${state_failure_scenario}.c)

    set(state_failure_target test_xr_program_aot_module_state_${state_failure_scenario})

    list(APPEND state_failure_targets)
endforeach()
