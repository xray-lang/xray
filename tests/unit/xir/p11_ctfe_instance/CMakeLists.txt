

add_test(NAME test_xir_p11_comptime_binding_wire_model COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../derive_combined_wire.py" --source-root "${PROJECT_SOURCE_DIR}" --output-root "${PROJECT_SOURCE_DIR}")
set_tests_properties(test_xir_p11_comptime_binding_wire_model PROPERTIES TIMEOUT 30)


