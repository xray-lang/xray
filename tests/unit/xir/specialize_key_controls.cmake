# Long vectors and literal call order provide independent specialization keys.
add_test(NAME test_xir_specialize_key_controls
    COMMAND test_xir_source_generic_callable_context --spec-key-controls)
set_tests_properties(test_xir_specialize_key_controls PROPERTIES
    LABELS "unit;xir;specialization;ownership" TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)
