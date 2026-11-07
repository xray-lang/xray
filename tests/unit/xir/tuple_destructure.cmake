

add_test(NAME test_xir_tuple_slot_group_current_model COMMAND ${XRAY_PYTHON}
    ${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_combined_wire.py
    --check ${CMAKE_CURRENT_SOURCE_DIR}/xir/slot_group76_golden.h)
add_test(NAME test_xir_tuple_slot_group_previous_model COMMAND ${XRAY_PYTHON}
    ${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_slot_group65_golden.py
    --check ${CMAKE_CURRENT_SOURCE_DIR}/xir/slot_group65_golden.h)
set_tests_properties(test_xir_tuple_slot_group_current_model test_xir_tuple_slot_group_previous_model
    PROPERTIES LABELS "unit;xir;tuple;checked;golden;generated" TIMEOUT 30)
