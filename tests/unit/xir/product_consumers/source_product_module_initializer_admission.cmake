# These positive compiler gates are prerequisites for the original runtime failures.
set(initializer_admission_root "${CMAKE_CURRENT_LIST_DIR}/module_initializer_admission")
set(initializer_admission_output "${CMAKE_BINARY_DIR}/consumer-initializer-admission")
file(MAKE_DIRECTORY "${initializer_admission_output}")
foreach(case IN ITEMS error panic panic_message imported_scalar_slot imported_class_slot imported_local_class)
    add_test(NAME test_source_product_module_initializer_admission_${case}
        COMMAND test_source_product_entry_roles_probe "initializer_${case}"
            "${initializer_admission_root}/${case}" "${initializer_admission_root}/${case}/root.xr"
            "${initializer_admission_output}/${case}.c")
    set_tests_properties(test_source_product_module_initializer_admission_${case}
        PROPERTIES TIMEOUT 120 PROCESSORS 1
        LABELS "unit;xir;source-product;program-consumer;initializer-admission")
endforeach()
add_test(NAME source_product_module_initializer_inputs_preserved
    COMMAND ${XRAY_PYTHON} -B -X utf8 "${CMAKE_SOURCE_DIR}/scripts/source_product_consumer_initializer_inputs.py"
        --root "${CMAKE_SOURCE_DIR}")
set_tests_properties(source_product_module_initializer_inputs_preserved
    PROPERTIES TIMEOUT 120 PROCESSORS 1 LABELS "unit;xir;program-consumer;inventory")
