add_test(NAME test_xir_calls_owned_recovery
    COMMAND test_xir_calls --owned-recovery)
set_tests_properties(test_xir_calls_owned_recovery PROPERTIES
    TIMEOUT 60 LABELS "unit;xir;program;ownership;execution;atomic-receive")
