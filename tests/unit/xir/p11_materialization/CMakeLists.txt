

add_test(NAME test_xir_materialization_wire_vector COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../derive_combined_wire.py" --source-root "${PROJECT_SOURCE_DIR}" --output-root "${PROJECT_SOURCE_DIR}")
set_tests_properties(test_xir_materialization_wire_vector PROPERTIES TIMEOUT 30 LABELS "unit;xir;checked;materialization")
add_test(NAME test_xir_comptime_library_wire_vector COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../derive_combined_wire.py" --source-root "${PROJECT_SOURCE_DIR}" --output-root "${PROJECT_SOURCE_DIR}")
set_tests_properties(test_xir_comptime_library_wire_vector PROPERTIES TIMEOUT 30 LABELS "unit;xir;checked;independent")
if(WIN32)
    add_test(NAME test_xir_materialization_construction_interface COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/verify_construction_interface.py" --archive "$<TARGET_FILE:xray_xir_admission>" --header "${PROJECT_SOURCE_DIR}/src/xir/xxir_construction.h")
    set_tests_properties(test_xir_materialization_construction_interface PROPERTIES TIMEOUT 30 LABELS "unit;xir;compiler;abi;materialization")
endif()
xray_stage_windows_asan_runtime("${CMAKE_CURRENT_BINARY_DIR}")
xray_prepend_windows_asan_runtime_to_directory_tests()
