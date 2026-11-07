# Four-slot class initialization preserves actual identity and allocator releases.

set(state_class_targets test_xir_module_state_classes)
foreach(state_class_scenario IN ITEMS 5 8)
    set(state_class_c ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_module_state_class_${state_class_scenario}.c)

    set(state_class_target test_xr_program_aot_module_state_${state_class_scenario})

    list(APPEND state_class_targets)
endforeach()
