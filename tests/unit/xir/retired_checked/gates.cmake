add_executable(test_xir_retired_checked "${CMAKE_CURRENT_LIST_DIR}/test_retired_checked.c")
target_link_libraries(test_xir_retired_checked PRIVATE xray_xir_admission xray_xir_scalar)
add_test(NAME test_xir_retired_checked COMMAND test_xir_retired_checked)
add_test(NAME test_xir_retired_checked_budget_prefixes COMMAND test_xir_retired_checked --budget-prefixes)
set_tests_properties(test_xir_retired_checked test_xir_retired_checked_budget_prefixes PROPERTIES
    TIMEOUT 60 RUN_SERIAL TRUE LABELS "unit;xir;checked;wire;ownership;resources;independent")
if(Python3_EXECUTABLE)
    add_test(NAME test_xir_retired_checked_originals
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_LIST_DIR}/derive.py")
    set_tests_properties(test_xir_retired_checked_originals PROPERTIES
        TIMEOUT 60 RUN_SERIAL TRUE LABELS "unit;xir;checked;wire;independent")
endif()
