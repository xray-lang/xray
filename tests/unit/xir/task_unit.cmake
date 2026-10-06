add_executable(test_xir_task_unit xir/test_xir_task_unit.c)
target_link_libraries(test_xir_task_unit PRIVATE xray_xir_source xray_xir_vm)
target_compile_definitions(test_xir_task_unit PRIVATE
    XR_TASK_UNIT_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_task_unit"
    XR_SOURCE_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
if(MSVC)
    target_compile_options(test_xir_task_unit PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_task_unit PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_task_unit COMMAND test_xir_task_unit)
set_tests_properties(test_xir_task_unit PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE PROCESSORS 1
    LABELS "unit;xir;task;source;runtime;ownership")
