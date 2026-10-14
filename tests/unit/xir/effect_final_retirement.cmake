add_test(NAME test_xir_effect_final_retirement
    COMMAND test_xir_root_conditional_helpers --final-retirement)
set_tests_properties(test_xir_effect_final_retirement PROPERTIES
    LABELS "unit;xir;root-effects;ownership;compiler-fault;final-retirement"
    TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)
