add_executable(test_xir_mixed_task_result_producer xir/test_xir_mixed_task_result_producer.c)
target_link_libraries(test_xir_mixed_task_result_producer PRIVATE xray_xir_source xray_xir_cgen)
target_compile_definitions(test_xir_mixed_task_result_producer PRIVATE
    XR_MIXED_TASK_RESULT_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p20_task_results")
set(XIR_MIXED_TASK_RESULT_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_mixed_task_results.c")
add_custom_command(OUTPUT "${XIR_MIXED_TASK_RESULT_C}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_mixed_task_result_producer> "${XIR_MIXED_TASK_RESULT_C}"
    DEPENDS test_xir_mixed_task_result_producer
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p20_task_results/root.xr"
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p20_task_results/worker.xr"
    VERBATIM)
add_executable(test_xir_mixed_task_result_lifetime xir/test_xir_mixed_task_result_lifetime.c "${XIR_MIXED_TASK_RESULT_C}")
target_link_libraries(test_xir_mixed_task_result_lifetime PRIVATE xray_xir_vm xray_xir_scalar)
foreach(task_result_target IN ITEMS test_xir_mixed_task_result_producer test_xir_mixed_task_result_lifetime)
    set_target_properties(${task_result_target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${task_result_target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${task_result_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
foreach(task_result_backend IN ITEMS 0 1 vm native)
    add_test(NAME test_xir_mixed_task_result_lifetime_${task_result_backend}
        COMMAND test_xir_mixed_task_result_lifetime ${task_result_backend})
    set_tests_properties(test_xir_mixed_task_result_lifetime_${task_result_backend} PROPERTIES
        TIMEOUT 120 RUN_SERIAL TRUE COST 1 LABELS "unit;xir;mixed;task;ownership;generated-c;lifecycle;compound")
endforeach()
