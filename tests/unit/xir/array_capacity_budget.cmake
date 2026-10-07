# The existing capacity targets own the complete Source-to-Program pipeline.
foreach(mode RANGE 0 3)
    if(mode EQUAL 0)
        set(capacity_budget_target test_xir_array_capacity)
    else()
        set(capacity_budget_target test_xir_array_capacity_native)
    endif()
    add_test(NAME test_xir_array_capacity_runtime_budget_${mode}
        COMMAND ${capacity_budget_target} --runtime-budget ${mode})
    set_tests_properties(test_xir_array_capacity_runtime_budget_${mode}
        PROPERTIES TIMEOUT 120 LABELS "unit;xir;array;ownership;memory;cancel;budget")
endforeach()
