add_executable(test_xir_cell_loan_lifecycle xir/test_xir_cell_loan_lifecycle.c)
target_link_libraries(test_xir_cell_loan_lifecycle PRIVATE xray_xir_source xray_xir_vm)
target_compile_definitions(test_xir_cell_loan_lifecycle PRIVATE
    XR_CELL_OWNER_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_cell_owner")
add_test(NAME test_xir_cell_loan_lifecycle COMMAND test_xir_cell_loan_lifecycle)
set_tests_properties(test_xir_cell_loan_lifecycle PROPERTIES LABELS "unit;xir;ownership;source;lifecycle" TIMEOUT 120)

# Each resource mode preserves all nine direct and all nine indirect Source cases.
foreach(mode IN ITEMS normal fi axes prepare)
    add_test(NAME test_xir_cell_loan_lifecycle_runtime_${mode}
        COMMAND test_xir_cell_loan_lifecycle --runtime-${mode})
    set_tests_properties(test_xir_cell_loan_lifecycle_runtime_${mode} PROPERTIES
        LABELS "unit;xir;ownership;source;lifecycle;resources" TIMEOUT 120)
endforeach()
