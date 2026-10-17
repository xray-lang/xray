add_executable(test_xir_snapshot_delta_mask "${CMAKE_CURRENT_LIST_DIR}/test_xir_snapshot_delta_mask.c")
target_include_directories(test_xir_snapshot_delta_mask PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(test_xir_snapshot_delta_mask PRIVATE xray_xir xray_xir_scalar xray_xir_source)
set_target_properties(test_xir_snapshot_delta_mask PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_snapshot_delta_mask PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_xir_snapshot_delta_mask PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_snapshot_delta_mask COMMAND test_xir_snapshot_delta_mask)
set_tests_properties(test_xir_snapshot_delta_mask PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE PROCESSORS 1
    LABELS "unit;xir;compiler;ownership;metadata")

# Keep the previously queued helper executable intact while rebuilding the
# unchanged original fixtures against the current compiler implementation.
add_executable(test_xir_snapshot_read_controls "${CMAKE_CURRENT_LIST_DIR}/test_xir_snapshot_read_controls.c")
target_include_directories(test_xir_snapshot_read_controls PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(test_xir_snapshot_read_controls PRIVATE xray_xir xray_xir_scalar xray_xir_source)
set_target_properties(test_xir_snapshot_read_controls PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_snapshot_read_controls PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_xir_snapshot_read_controls PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_snapshot_origin_controls COMMAND test_xir_snapshot_read_controls --origin-operand-literals)
add_test(NAME test_xir_snapshot_existing_controls COMMAND test_xir_snapshot_read_controls --existing-node-literals)
add_test(NAME test_xir_snapshot_cell_controls COMMAND test_xir_snapshot_read_controls --cell-literals)
set_tests_properties(test_xir_snapshot_origin_controls test_xir_snapshot_existing_controls test_xir_snapshot_cell_controls PROPERTIES
    TIMEOUT 60 RUN_SERIAL TRUE PROCESSORS 1 LABELS "unit;xir;compiler;ownership;metadata")
