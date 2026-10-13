# Keep the full cleanup/FI registration and add independent late-event oracles.
foreach(native_cleanup IN ITEMS 0 1)
    add_test(NAME test_xir_mixed_lifecycle_${native_cleanup}
        COMMAND test_xir_cleanup_mixed --lifecycle ${native_cleanup})
    set_tests_properties(test_xir_mixed_lifecycle_${native_cleanup} PROPERTIES
        TIMEOUT 120 RUN_SERIAL TRUE COST 1
        LABELS "unit;xir;mixed;execution;ownership;instance;abi;lifecycle")
endforeach()
