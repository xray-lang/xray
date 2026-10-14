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
