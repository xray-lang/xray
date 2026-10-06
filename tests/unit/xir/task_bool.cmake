add_executable(test_xir_task_bool xir/test_xir_task_bool.c)
target_link_libraries(test_xir_task_bool PRIVATE xray_xir_source xray_xir_vm)
target_compile_definitions(test_xir_task_bool PRIVATE
    XR_TASK_BOOL_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_task_bool"
    XR_SOURCE_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
if(MSVC)
    target_compile_options(test_xir_task_bool PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_task_bool PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_task_bool COMMAND test_xir_task_bool)
set_tests_properties(test_xir_task_bool PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE PROCESSORS 1
    LABELS "unit;xir;task;source;runtime;ownership")
