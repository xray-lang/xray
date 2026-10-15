if(TARGET test_lsp_source_task)
    return()
endif()
function(source_task_target target navigation)
if(ARGC GREATER 2)
    set(task_owner "${ARGV2}")
else()
    set(task_owner "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/task_owner.c")
endif()
add_executable(${target} "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/test_task.c"
    "${task_owner}"
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/identity_owner.c"
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/worker_owner.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_source_completion.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_pending_requests.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_request_cancel.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_transport.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_framing.c"
    "${navigation}"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_source_scope.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_source_binding_lenses.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_source_bindings.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_source_index.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_source_rename.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_ignore.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_index_scan.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_index_pool.c")
target_link_libraries(${target} PRIVATE xray_lsp_document_open xray_lsp_hierarchy_handlers)
if(WIN32)
    target_sources(${target} PRIVATE "${PROJECT_SOURCE_DIR}/src/os/win/thread_win.c"
        "${PROJECT_SOURCE_DIR}/src/os/win/pipe_win.c" "${PROJECT_SOURCE_DIR}/src/os/win/fd_win.c"
        "${PROJECT_SOURCE_DIR}/src/os/win/time_win.c")
    target_link_libraries(${target} PRIVATE bcrypt)
else()
    target_sources(${target} PRIVATE "${PROJECT_SOURCE_DIR}/src/os/unix/thread_unix.c"
        "${PROJECT_SOURCE_DIR}/src/os/unix/pipe_unix.c" "${PROJECT_SOURCE_DIR}/src/os/unix/fd_unix.c"
        "${PROJECT_SOURCE_DIR}/src/os/unix/time_unix.c")
endif()
target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/src/base" "${PROJECT_SOURCE_DIR}/src/app/lsp" "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/tests/unit/xir")
target_compile_features(${target} PRIVATE c_std_11)
if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
endif()
endfunction()
source_task_target(test_lsp_source_task "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_navigation.c")
source_task_target(test_lsp_source_task_before "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_navigation.c")
source_task_target(test_lsp_source_task_budget_before "${CMAKE_CURRENT_LIST_DIR}/before_navigation.c")
target_sources(test_lsp_source_task_before PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/xir/source_cancel/before_request.c")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME test_lsp_source_task COMMAND "${Python3_EXECUTABLE}" -X utf8
    "${CMAKE_CURRENT_LIST_DIR}/test_task.py" "$<TARGET_FILE:test_lsp_source_task>" "${PROJECT_SOURCE_DIR}/stdlib")
set_tests_properties(test_lsp_source_task PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE LABELS "unit;lsp;source;ownership")

add_test(NAME test_lsp_source_task_counterfactual COMMAND "${Python3_EXECUTABLE}" -X utf8
    "${CMAKE_CURRENT_LIST_DIR}/test_task.py" "$<TARGET_FILE:test_lsp_source_task_before>" "${PROJECT_SOURCE_DIR}/stdlib" --before)
set_tests_properties(test_lsp_source_task_counterfactual PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE LABELS "unit;lsp;source;ownership")
add_test(NAME test_lsp_source_task_budget_counterfactual COMMAND "${Python3_EXECUTABLE}" -X utf8
    "${CMAKE_CURRENT_LIST_DIR}/test_task.py" "$<TARGET_FILE:test_lsp_source_task_budget_before>" "${PROJECT_SOURCE_DIR}/stdlib" --budget-before)
set_tests_properties(test_lsp_source_task_budget_counterfactual PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE LABELS "unit;lsp;source;ownership")
