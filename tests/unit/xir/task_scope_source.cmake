set(task_scope_source_c "${CMAKE_CURRENT_BINARY_DIR}/generated/task_scope_source.c")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_executable(test_xir_task_scope_source xir/test_xir_task_scope_source.c)
target_link_libraries(test_xir_task_scope_source PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
add_custom_command(OUTPUT "${task_scope_source_c}"
    COMMAND $<TARGET_FILE:test_xir_task_scope_source> --emit "${task_scope_source_c}"
    DEPENDS test_xir_task_scope_source "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_task_scope_source/scope.xr"
 VERBATIM)
add_executable(test_xir_task_scope_source_native xir/test_xir_task_scope_source.c "${task_scope_source_c}")
target_compile_definitions(test_xir_task_scope_source_native PRIVATE XR_TASK_SCOPE_SOURCE_NATIVE=1)
target_link_libraries(test_xir_task_scope_source_native PRIVATE xray_xir_scalar)
foreach(target test_xir_task_scope_source test_xir_task_scope_source_native)
    target_link_libraries(${target} PRIVATE xray_xir_runtime_host)
    target_sources(${target} PRIVATE "${PROJECT_SOURCE_DIR}/${AOT_TIME_SRC}")
    target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_compile_definitions(${target} PRIVATE
        XR_TASK_SCOPE_SOURCE_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_task_scope_source"
        XR_SOURCE_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME xir_task_scope_source COMMAND test_xir_task_scope_source)
add_test(NAME xir_task_scope_source_native COMMAND test_xir_task_scope_source_native)
set_tests_properties(xir_task_scope_source xir_task_scope_source_native PROPERTIES TIMEOUT 120 LABELS "unit;xir;task;source;ownership")
