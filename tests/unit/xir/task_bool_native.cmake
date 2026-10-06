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
add_executable(test_xir_task_bool_native "${CMAKE_CURRENT_LIST_DIR}/test_xir_task_bool_native.c"
    "${task_bool_generated}/go_bool_native.c" "${task_bool_generated}/go_bool_generated.h"
    "${PROJECT_SOURCE_DIR}/src/base/xsha256.c" "${PROJECT_SOURCE_DIR}/src/shared/xnative_declaration.c")
target_include_directories(test_xir_task_bool_native PRIVATE ${XRAY_COMMON_INCLUDES}
    "${CMAKE_CURRENT_LIST_DIR}" "${task_bool_generated}")
target_link_libraries(test_xir_task_bool_native PRIVATE kernel32)
foreach(target test_xir_task_bool_native_producer test_xir_task_bool_native)
    target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
endforeach()
add_test(NAME test_xir_task_bool_native COMMAND test_xir_task_bool_native)
set_tests_properties(test_xir_task_bool_native PROPERTIES TIMEOUT 60 LABELS "ownership")
