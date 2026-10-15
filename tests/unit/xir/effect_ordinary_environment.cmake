add_test(NAME test_xir_effect_ordinary_environment_keys
    COMMAND test_xir_root_conditional_helpers --ordinary-environment-keys)
set_tests_properties(test_xir_effect_ordinary_environment_keys PROPERTIES
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1
    LABELS "unit;xir;ownership;metadata;compiler-fault;ordinary-context")

add_test(NAME test_xir_effect_source_function_capacity
    COMMAND test_xir_source_query --requirement-capacity)
add_test(NAME test_xir_effect_source_input_provenance
    COMMAND test_xir_source_query --requirement-provenance)
set_tests_properties(test_xir_effect_source_function_capacity test_xir_effect_source_input_provenance PROPERTIES
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1
    LABELS "unit;xir;ownership;metadata;ordinary-context;final-input")

add_test(NAME test_xir_effect_ordinary_environment_literals
    COMMAND test_xir_root_conditional_helpers --ordinary-environment-literals)
set_tests_properties(test_xir_effect_ordinary_environment_literals PROPERTIES
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1
    LABELS "unit;xir;ownership;metadata;ordinary-context;final-input")
