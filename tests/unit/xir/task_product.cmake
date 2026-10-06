add_executable(test_xir_task_vm_product xir/test_xir_task_vm_product.c)
target_link_libraries(test_xir_task_vm_product PRIVATE xray_xir_vm xray_xir_scalar)
add_executable(test_xir_task_root_fault xir/test_xir_task_root_fault.c)
target_link_libraries(test_xir_task_root_fault PRIVATE xray_xir_vm xray_xir_scalar)
add_executable(test_xir_task_root_retry xir/test_xir_task_root_retry.c)
target_link_libraries(test_xir_task_root_retry PRIVATE xray_xir_vm xray_xir_scalar xray_xir_cgen)
add_executable(test_xir_task_root_retry_probe xir/test_xir_task_root_retry_probe.c)
target_link_libraries(test_xir_task_root_retry_probe PRIVATE xray_xir_vm xray_xir_scalar)
add_executable(test_xir_go_cgen_source xir/test_xir_go_cgen_source.c)
target_link_libraries(test_xir_go_cgen_source PRIVATE xray_xir_cgen)
add_executable(test_xir_go_source_execution xir/test_xir_go_source_execution.c)
target_link_libraries(test_xir_go_source_execution PRIVATE xray_xir_source xray_xir_vm)
target_compile_definitions(test_xir_go_source_execution PRIVATE
    XR_GO_EXECUTION_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_go_execution"
    XR_SOURCE_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
add_executable(test_xir_go_source_product xir/test_xir_go_source_product.c)
target_link_libraries(test_xir_go_source_product PRIVATE xray_xir_source_product)
target_compile_definitions(test_xir_go_source_product PRIVATE
    XR_GO_PRODUCT_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_go_product"
    XR_SOURCE_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
foreach(target test_xir_task_vm_product test_xir_task_root_fault test_xir_task_root_retry test_xir_task_root_retry_probe test_xir_go_cgen_source test_xir_go_source_execution test_xir_go_source_product)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_xir_task_vm_product COMMAND test_xir_task_vm_product)
set_tests_properties(test_xir_task_vm_product PROPERTIES LABELS "unit;xir;task;ownership;runtime" TIMEOUT 60)
add_test(NAME test_xir_task_root_fault COMMAND test_xir_task_root_fault)
set_tests_properties(test_xir_task_root_fault PROPERTIES LABELS "unit;xir;task;ownership;runtime" TIMEOUT 60)
add_test(NAME test_xir_go_source_execution COMMAND test_xir_go_source_execution)
set_tests_properties(test_xir_go_source_execution PROPERTIES LABELS "unit;xir;task;source;ownership;runtime" TIMEOUT 60)
add_test(NAME test_xir_go_source_product COMMAND test_xir_go_source_product)
set_tests_properties(test_xir_go_source_product PROPERTIES LABELS "unit;xir;task;source;ownership;runtime" TIMEOUT 60)
foreach(case integer string generic context_ordinary context_go context_existing context_spawn grouped
    import_default const_state local_storage unknown_task value_error shadow_class context_match
    context_match_block context_match_existing)
    add_test(NAME test_xir_go_source_runtime_oom_${case}
        COMMAND test_xir_go_source_execution --runtime-oom ${case})
    add_test(NAME test_xir_go_source_runtime_axes_${case}
        COMMAND test_xir_go_source_execution --runtime-axes ${case})
    set_tests_properties(test_xir_go_source_runtime_oom_${case} test_xir_go_source_runtime_axes_${case}
        PROPERTIES LABELS "unit;xir;task;source;ownership;runtime" TIMEOUT 300)
endforeach()

add_test(NAME test_xir_task_root_retry COMMAND test_xir_task_root_retry)
set_tests_properties(test_xir_task_root_retry PROPERTIES LABELS "unit;xir;task;ownership;runtime" TIMEOUT 60)
include(${CMAKE_CURRENT_LIST_DIR}/task_fault_cleanup.cmake)
