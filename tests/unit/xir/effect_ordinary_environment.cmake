add_test(NAME test_xir_effect_ordinary_environment_keys
    COMMAND test_xir_root_conditional_helpers --ordinary-environment-keys)
set_tests_properties(test_xir_effect_ordinary_environment_keys PROPERTIES
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1
    LABELS "unit;xir;ownership;metadata;compiler-fault;ordinary-context")
