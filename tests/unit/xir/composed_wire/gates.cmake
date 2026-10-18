add_executable(test_xir_composed_wire "${CMAKE_CURRENT_LIST_DIR}/test_composed_wire.c")
target_link_libraries(test_xir_composed_wire PRIVATE xray_xir_admission xray_xir_scalar)
add_test(NAME test_xir_composed_wire COMMAND test_xir_composed_wire)
set_tests_properties(test_xir_composed_wire PROPERTIES
    TIMEOUT 60 RUN_SERIAL TRUE LABELS "unit;xir;checked;wire;independent")
if(Python3_EXECUTABLE)
    add_test(NAME test_xir_composed_wire_model
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_LIST_DIR}/derive.py")
    set_tests_properties(test_xir_composed_wire_model PROPERTIES
        TIMEOUT 30 LABELS "unit;xir;checked;wire;independent")
endif()
