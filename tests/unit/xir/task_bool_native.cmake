# Genuine Source-to-C pipeline and independent public native ownership checks.
set(task_bool_fixture "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_task_bool_native")
add_executable(test_xir_task_bool_native_producer "${CMAKE_CURRENT_LIST_DIR}/test_xir_task_bool_native_producer.c")
target_link_libraries(test_xir_task_bool_native_producer PRIVATE xray_xir_source xray_xir_cgen)
target_include_directories(test_xir_task_bool_native_producer PRIVATE ${XRAY_COMMON_INCLUDES} "${CMAKE_CURRENT_LIST_DIR}")
target_compile_definitions(test_xir_task_bool_native_producer PRIVATE
    XR_GO_BOOL_FIXTURES="${task_bool_fixture}" XR_SOURCE_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
set(task_bool_generated "${CMAKE_CURRENT_BINARY_DIR}/task_bool_generated")
add_custom_command(OUTPUT "${task_bool_generated}/go_bool_native.c" "${task_bool_generated}/go_bool_generated.h"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${task_bool_generated}"
    COMMAND test_xir_task_bool_native_producer "${task_bool_generated}/go_bool_native.c" "${task_bool_generated}/go_bool_generated.h"
    DEPENDS test_xir_task_bool_native_producer "${task_bool_fixture}/bool_native.xr" VERBATIM)

foreach(target IN ITEMS test_xir_task_bool_native_producer)
    target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
endforeach()
