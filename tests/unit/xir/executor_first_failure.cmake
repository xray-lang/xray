add_executable(test_xir_executor_first_failure xir/test_xir_executor_first_failure.c)
target_link_libraries(test_xir_executor_first_failure PRIVATE xray_xir_vm xray_xir_scalar)
if(MSVC)
    target_compile_options(test_xir_executor_first_failure PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_executor_first_failure PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_executor_first_failure COMMAND test_xir_executor_first_failure)
set_tests_properties(test_xir_executor_first_failure PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1
    LABELS "unit;xir;task;runtime;ownership;resource-budget")
add_test(NAME test_xir_first_failure_work_minus
    COMMAND test_xir_vm19_fault_oracle 0 18446744073709551615 2 28 4294967295)
set_tests_properties(test_xir_first_failure_work_minus PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1
    LABELS "unit;xir;task;runtime;ownership;resource-budget")
