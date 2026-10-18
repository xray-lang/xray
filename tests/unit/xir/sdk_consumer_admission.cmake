function(xir_register_sdk_consumer_admission python lease bundle compiler linker)
    add_test(NAME test_xir_runtime_sdk_consumer_admission
        COMMAND "${python}" -B "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/sdk_native_consumer_admission.py"
            --lease "${lease}" --bundle "${bundle}" --compiler "${compiler}" --linker "${linker}"
            --output "${CMAKE_CURRENT_BINARY_DIR}/sdk-consumer-admission-evidence")
    set_tests_properties(test_xir_runtime_sdk_consumer_admission PROPERTIES
        LABELS "unit;xir;execution;ownership;sdk;abi;admission"
        RUN_SERIAL TRUE TIMEOUT 300 COST 1)
endfunction()
