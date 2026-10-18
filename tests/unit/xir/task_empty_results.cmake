add_executable(test_xir_task_empty_emit ${CMAKE_CURRENT_LIST_DIR}/test_xir_task_constructed_emit.c)
target_link_libraries(test_xir_task_empty_emit PRIVATE xray_xir_source xray_xir_cgen xray_xir_vm)
target_compile_definitions(test_xir_task_empty_emit PRIVATE
    XR_TASK_CONSTRUCTED_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p08_task_empty_results"
    XR_SOURCE_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
if(MSVC)
    target_compile_options(test_xir_task_empty_emit PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_task_empty_emit PRIVATE -Wall -Wextra -Werror)
endif()
set(EMPTY_RESULT_DIR "${CMAKE_CURRENT_BINARY_DIR}/task-empty-results")
set(EMPTY_RESULT_C "${EMPTY_RESULT_DIR}/task_constructed_native.c")
set(EMPTY_RESULT_HEADER "${EMPTY_RESULT_DIR}/task_constructed_generated.h")
add_custom_command(OUTPUT "${EMPTY_RESULT_C}" "${EMPTY_RESULT_HEADER}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${EMPTY_RESULT_DIR}"
    COMMAND test_xir_task_empty_emit "${EMPTY_RESULT_C}" "${EMPTY_RESULT_HEADER}"
    DEPENDS test_xir_task_empty_emit "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p08_task_empty_results/root.xr" VERBATIM)
add_executable(test_xir_task_empty_results ${CMAKE_CURRENT_LIST_DIR}/test_xir_task_empty_results.c
    "${EMPTY_RESULT_C}" "${EMPTY_RESULT_HEADER}")
target_include_directories(test_xir_task_empty_results PRIVATE "${EMPTY_RESULT_DIR}")
target_link_libraries(test_xir_task_empty_results PRIVATE xray_xir_source xray_xir_vm)
target_compile_definitions(test_xir_task_empty_results PRIVATE
    XR_TASK_CONSTRUCTED_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p08_task_empty_results"
    XR_SOURCE_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
if(MSVC)
    target_compile_options(test_xir_task_empty_results PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_task_empty_results PRIVATE -Wall -Wextra -Werror)
endif()
foreach(family array nested nullable nullable_array tuple enum atomic task)
    foreach(backend vm native vm-native native-vm)
        string(REPLACE "-" "_" name "${backend}")
        foreach(mode normal fi)
            add_test(NAME test_xir_task_empty_${family}_${name}_${mode}
                COMMAND test_xir_task_empty_results ${family} ${backend} ${mode})
            set_tests_properties(test_xir_task_empty_${family}_${name}_${mode} PROPERTIES TIMEOUT 60
                LABELS "unit;xir;task;ownership;runtime;generated-c")
        endforeach()
    endforeach()
endforeach()
