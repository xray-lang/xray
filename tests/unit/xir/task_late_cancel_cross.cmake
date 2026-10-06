add_executable(test_xir_task_late_cancel_cross ${CMAKE_CURRENT_LIST_DIR}/test_xir_task_late_cancel_cross.c)
target_include_directories(test_xir_task_late_cancel_cross PRIVATE ${PROJECT_SOURCE_DIR}/tests/unit/xir)
target_link_libraries(test_xir_task_late_cancel_cross PRIVATE xray_xir_source xray_xir_vm)
target_compile_definitions(test_xir_task_late_cancel_cross PRIVATE
    XR_TASK_UNIT_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_task_unit"
    XR_SOURCE_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
if(MSVC)
    target_compile_options(test_xir_task_late_cancel_cross PRIVATE /W4 /WX /utf-8 /std:c11)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_xir_task_late_cancel_cross PRIVATE /experimental:c11atomics)
    endif()
else()
    target_compile_options(test_xir_task_late_cancel_cross PRIVATE -Wall -Wextra -Werror -std=c11)
endif()
if(WIN32)
    add_test(NAME test_xir_task_late_cancel_cross
        COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_LIST_DIR}/run_task_late_cancel_cross.py
            --exe $<TARGET_FILE:test_xir_task_late_cancel_cross>
            --output ${CMAKE_CURRENT_BINARY_DIR}/task-late-cancel-cross --phase normal)
    set_tests_properties(test_xir_task_late_cancel_cross PROPERTIES
        TIMEOUT 60 RUN_SERIAL TRUE PROCESSORS 1 LABELS "unit;xir;task;source;runtime;ownership")
endif()
