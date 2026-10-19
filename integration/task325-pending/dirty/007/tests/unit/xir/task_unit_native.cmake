# Unit Tasks are generated from the original eight owned Source fixtures.
set(TASK_UNIT_NATIVE_NAMES inferred explicit_unit explicit_null generic escape unit_root unknown_waiter escaped_error)
set(TASK_UNIT_NATIVE_FIXTURE_DIR "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_task_unit")
add_executable(test_xir_task_unit_native_producer "${CMAKE_CURRENT_LIST_DIR}/test_xir_task_unit_native_producer.c")
target_link_libraries(test_xir_task_unit_native_producer PRIVATE xray_xir_source xray_xir_cgen)
target_include_directories(test_xir_task_unit_native_producer PRIVATE ${XRAY_COMMON_INCLUDES} "${CMAKE_CURRENT_LIST_DIR}")
target_compile_definitions(test_xir_task_unit_native_producer PRIVATE
    XR_TASK_UNIT_FIXTURES="${TASK_UNIT_NATIVE_FIXTURE_DIR}" XR_SOURCE_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
set(TASK_UNIT_NATIVE_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/task_unit_native")
set(TASK_UNIT_NATIVE_CS)
set(TASK_UNIT_NATIVE_FIXTURES)
foreach(case IN LISTS TASK_UNIT_NATIVE_NAMES)
    list(APPEND TASK_UNIT_NATIVE_CS "${TASK_UNIT_NATIVE_GENERATED_DIR}/task_unit_native_${case}.c")
    list(APPEND TASK_UNIT_NATIVE_FIXTURES "${TASK_UNIT_NATIVE_FIXTURE_DIR}/${case}.xr")
endforeach()
add_custom_command(OUTPUT ${TASK_UNIT_NATIVE_CS} "${TASK_UNIT_NATIVE_GENERATED_DIR}/task_unit_generated.h"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${TASK_UNIT_NATIVE_GENERATED_DIR}"
    COMMAND test_xir_task_unit_native_producer "${TASK_UNIT_NATIVE_GENERATED_DIR}"
    DEPENDS test_xir_task_unit_native_producer ${TASK_UNIT_NATIVE_FIXTURES} VERBATIM)
add_executable(test_xir_task_unit_native "${CMAKE_CURRENT_LIST_DIR}/test_xir_task_unit_native.c"
    ${TASK_UNIT_NATIVE_CS} "${TASK_UNIT_NATIVE_GENERATED_DIR}/task_unit_generated.h"
    "${PROJECT_SOURCE_DIR}/src/base/xsha256.c" "${PROJECT_SOURCE_DIR}/src/shared/xnative_declaration.c")
target_include_directories(test_xir_task_unit_native PRIVATE ${XRAY_COMMON_INCLUDES}
    "${CMAKE_CURRENT_LIST_DIR}" "${TASK_UNIT_NATIVE_GENERATED_DIR}")
if(WIN32)
    target_link_libraries(test_xir_task_unit_native PRIVATE kernel32)
endif()
foreach(target test_xir_task_unit_native_producer test_xir_task_unit_native)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_xir_task_unit_native COMMAND test_xir_task_unit_native)
set_tests_properties(test_xir_task_unit_native PROPERTIES TIMEOUT 60 LABELS "ownership;unit;xir;task;native")
