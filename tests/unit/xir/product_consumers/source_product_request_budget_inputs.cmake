# Normal source controls do not exercise the missing legacy request budgets.
set(request_budget_input_root "${CMAKE_CURRENT_LIST_DIR}/request_budget_inputs")
foreach(case IN ITEMS module_count invalid_request mono_depth mono_instances graph_instances program_bytes)
    add_test(NAME test_source_product_request_budget_input_${case}
        COMMAND test_source_product_probe "request_budget_${case}"
            "${request_budget_input_root}/${case}" "${request_budget_input_root}/${case}/root.xr" 42 0)
    set_tests_properties(test_source_product_request_budget_input_${case}
        PROPERTIES TIMEOUT 120 PROCESSORS 1
        LABELS "unit;xir;source-product;program-consumer;budget-input-control")
endforeach()
add_test(NAME source_product_request_budget_inputs_preserved
    COMMAND ${XRAY_PYTHON} -B -X utf8 "${CMAKE_SOURCE_DIR}/scripts/source_product_consumer_request_budgets.py"
        --root "${CMAKE_SOURCE_DIR}")
set_tests_properties(source_product_request_budget_inputs_preserved
    PROPERTIES TIMEOUT 120 PROCESSORS 1 LABELS "unit;xir;program-consumer;inventory")
