add_executable(test_xir_task_small_runs "${CMAKE_CURRENT_LIST_DIR}/test_xir_task_small_runs.c")
target_link_libraries(test_xir_task_small_runs PRIVATE xray_xir xray_xir_scalar xray_xir_source)
if(MSVC)
    target_compile_options(test_xir_task_small_runs PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_task_small_runs PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_task_small_runs COMMAND test_xir_task_small_runs)
set_tests_properties(test_xir_task_small_runs PROPERTIES
    TIMEOUT 60 LABELS "unit;xir;task;ownership;source;root-effects;malformed-input")
foreach(owned_run_case IN ITEMS instances capture_index)
    add_test(NAME test_xir_root_parameter_${owned_run_case}_normal
        COMMAND test_xir_root_parameter_${owned_run_case} --normal)
    set_tests_properties(test_xir_root_parameter_${owned_run_case}_normal PROPERTIES
        TIMEOUT 600 LABELS "unit;xir;root-effects;higher-order;compiler;ownership;finite-control")
endforeach()
