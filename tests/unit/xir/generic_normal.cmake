# Each registration runs one complete original helper and its owner cleanup.
foreach(generic_normal_case IN ITEMS
    cross_pool_substitution
    nominal_ordered_matching
    nominal_field_closure
    nominal_function_closure
    nominal_expression_closure
    deep_body_substitution
    array_generic_cases
    array_definition_constraints
)
    add_test(NAME test_xir_generics_normal_${generic_normal_case}
        COMMAND test_xir_generics --normal-case ${generic_normal_case})
    set_tests_properties(test_xir_generics_normal_${generic_normal_case} PROPERTIES
        LABELS "unit;xir;generics;normal;ownership;abi" TIMEOUT 300 PROCESSORS 1)
endforeach()
