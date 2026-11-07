set(ARRAY_ERROR_NAMES array nested strings)
set(ARRAY_ERROR_DIR "${CMAKE_CURRENT_BINARY_DIR}/task-array-error")
add_executable(test_xir_task_array_error_emit ${CMAKE_CURRENT_LIST_DIR}/test_xir_task_array_error_emit.c)
target_link_libraries(test_xir_task_array_error_emit PRIVATE xray_xir_source xray_xir_cgen)
target_compile_definitions(test_xir_task_array_error_emit PRIVATE
    XR_TASK_UNIT_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p08_task_errors"
    XR_SOURCE_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
set(ARRAY_ERROR_C)
set(ARRAY_ERROR_FIXTURES)
foreach(case IN LISTS ARRAY_ERROR_NAMES)
    list(APPEND ARRAY_ERROR_C "${ARRAY_ERROR_DIR}/task_array_error_${case}.c")
    list(APPEND ARRAY_ERROR_FIXTURES "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p08_task_errors/${case}.xr")
endforeach()
add_custom_command(OUTPUT ${ARRAY_ERROR_C} "${ARRAY_ERROR_DIR}/task_array_error_generated.h"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ARRAY_ERROR_DIR}"
    COMMAND test_xir_task_array_error_emit "${ARRAY_ERROR_DIR}"
    DEPENDS test_xir_task_array_error_emit ${ARRAY_ERROR_FIXTURES} VERBATIM)

foreach(target IN ITEMS test_xir_task_array_error_emit)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
