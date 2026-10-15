set(CLOSED_ERROR_DIR "${CMAKE_CURRENT_BINARY_DIR}/task-closed-error")
add_executable(test_xir_task_closed_error_emit ${CMAKE_CURRENT_LIST_DIR}/test_xir_task_closed_error_emit.c)
target_link_libraries(test_xir_task_closed_error_emit PRIVATE xray_xir_source xray_xir_cgen xray_xir_vm)
set(CLOSED_ERROR_C "${CLOSED_ERROR_DIR}/task_closed_error_native.c")
set(CLOSED_ERROR_HEADER "${CLOSED_ERROR_DIR}/task_closed_error_generated.h")
add_custom_command(OUTPUT "${CLOSED_ERROR_C}" "${CLOSED_ERROR_HEADER}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CLOSED_ERROR_DIR}"
    COMMAND test_xir_task_closed_error_emit "${CLOSED_ERROR_C}" "${CLOSED_ERROR_HEADER}"
    DEPENDS test_xir_task_closed_error_emit "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p08_task_closed_errors/root.xr" VERBATIM)
add_executable(test_xir_task_closed_error_baseline ${CMAKE_CURRENT_LIST_DIR}/test_xir_task_closed_error_baseline.c
    "${CLOSED_ERROR_C}" "${CLOSED_ERROR_HEADER}")
target_include_directories(test_xir_task_closed_error_baseline PRIVATE "${CLOSED_ERROR_DIR}")
target_link_libraries(test_xir_task_closed_error_baseline PRIVATE xray_xir_source xray_xir_vm)
foreach(target test_xir_task_closed_error_emit test_xir_task_closed_error_baseline)
    target_compile_definitions(${target} PRIVATE
        XR_TASK_CLOSED_ERROR_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p08_task_closed_errors"
        XR_SOURCE_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_xir_task_closed_error_source_task_field COMMAND test_xir_task_closed_error_baseline source-task-field)
set_tests_properties(test_xir_task_closed_error_source_task_field PROPERTIES TIMEOUT 60
    LABELS "unit;xir;task;ownership;source;typed-error")
foreach(backend vm native vm-native native-vm)
    string(REPLACE "-" "_" name "${backend}")
    add_test(NAME test_xir_task_closed_error_baseline_${name} COMMAND test_xir_task_closed_error_baseline ${backend})
    set_tests_properties(test_xir_task_closed_error_baseline_${name} PROPERTIES TIMEOUT 60
        LABELS "unit;xir;task;ownership;runtime;generated-c;typed-error")
endforeach()
