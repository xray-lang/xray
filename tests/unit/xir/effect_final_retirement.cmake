add_test(NAME test_xir_effect_nonroot_input_seeds_resources
    COMMAND test_xir_root_conditional_helpers --nonroot-input-seeds-resources)
set_tests_properties(test_xir_effect_nonroot_input_seeds_resources PROPERTIES
    LABELS "unit;xir;root-effects;ownership;metadata;compiler-fault;final-input"
    TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)

add_test(NAME test_xir_effect_nonroot_input_seeds_literals
    COMMAND test_xir_root_conditional_helpers --nonroot-input-seeds-literals)
set_tests_properties(test_xir_effect_nonroot_input_seeds_literals PROPERTIES
    LABELS "unit;xir;root-effects;ownership;metadata;partial-feedback;final-input"
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1)

add_test(NAME test_xir_effect_final_retirement_literals
    COMMAND test_xir_root_conditional_helpers --final-retirement-literals)
set_tests_properties(test_xir_effect_final_retirement_literals PROPERTIES
    LABELS "unit;xir;root-effects;ownership;metadata;partial-feedback;final-retirement"
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1)

add_test(NAME test_xir_effect_final_retirement
    COMMAND test_xir_root_conditional_helpers --final-retirement)
set_tests_properties(test_xir_effect_final_retirement PROPERTIES
    LABELS "unit;xir;root-effects;ownership;compiler-fault;final-retirement"
    TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)
