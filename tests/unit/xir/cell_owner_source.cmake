# Real Source ownership checks use the existing finite compiler/runtime observers.
add_executable(test_xir_cell_owner_source xir/test_xir_cell_owner_source.c)
target_link_libraries(test_xir_cell_owner_source PRIVATE xray_xir_source xray_xir_vm)
target_compile_definitions(test_xir_cell_owner_source PRIVATE
    XR_CELL_OWNER_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_cell_owner")
add_test(NAME test_xir_cell_owner_source COMMAND test_xir_cell_owner_source)
set_tests_properties(test_xir_cell_owner_source PROPERTIES LABELS "unit;xir;ownership;source" TIMEOUT 120)
