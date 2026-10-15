if(TARGET test_lsp_source_task)
    return()
endif()
add_executable(test_lsp_source_task "${CMAKE_CURRENT_LIST_DIR}/test_task.c"
    "${CMAKE_CURRENT_LIST_DIR}/task_owner.c"
    "${CMAKE_CURRENT_LIST_DIR}/identity_owner.c"
    "${CMAKE_CURRENT_LIST_DIR}/worker_owner.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_source_completion.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_pending_requests.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_request_cancel.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_transport.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_framing.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_navigation.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_source_scope.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_source_binding_lenses.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_source_bindings.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_source_index.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_source_rename.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_ignore.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_index_scan.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_index_pool.c")
target_link_libraries(test_lsp_source_task PRIVATE xray_lsp_document_open)
if(WIN32)
    target_sources(test_lsp_source_task PRIVATE "${PROJECT_SOURCE_DIR}/src/os/win/thread_win.c"
        "${PROJECT_SOURCE_DIR}/src/os/win/pipe_win.c" "${PROJECT_SOURCE_DIR}/src/os/win/fd_win.c"
        "${PROJECT_SOURCE_DIR}/src/os/win/time_win.c")
    target_link_libraries(test_lsp_source_task PRIVATE bcrypt)
else()
    target_sources(test_lsp_source_task PRIVATE "${PROJECT_SOURCE_DIR}/src/os/unix/thread_unix.c"
        "${PROJECT_SOURCE_DIR}/src/os/unix/pipe_unix.c" "${PROJECT_SOURCE_DIR}/src/os/unix/fd_unix.c"
        "${PROJECT_SOURCE_DIR}/src/os/unix/time_unix.c")
endif()
target_include_directories(test_lsp_source_task PRIVATE "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/src/base" "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/tests/unit/xir")
target_compile_features(test_lsp_source_task PRIVATE c_std_11)
if(MSVC)
    target_compile_options(test_lsp_source_task PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_lsp_source_task PRIVATE -Wall -Wextra -Werror)
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME test_lsp_source_task COMMAND "${Python3_EXECUTABLE}" -X utf8
    "${CMAKE_CURRENT_LIST_DIR}/test_task.py" "$<TARGET_FILE:test_lsp_source_task>" "${PROJECT_SOURCE_DIR}/stdlib")
set_tests_properties(test_lsp_source_task PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE LABELS "unit;lsp;source;ownership")
