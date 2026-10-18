if(TARGET test_lsp_request_cancel)
    return()
endif()
add_executable(test_lsp_request_cancel "${CMAKE_CURRENT_LIST_DIR}/test_cancel.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_request_cancel.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_pending_requests.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_protocol_message.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_async.c"
    "${PROJECT_SOURCE_DIR}/src/base/xjson.c" "${PROJECT_SOURCE_DIR}/src/base/xutf8.c")
if(WIN32)
    target_sources(test_lsp_request_cancel PRIVATE "${PROJECT_SOURCE_DIR}/src/os/win/thread_win.c"
        "${PROJECT_SOURCE_DIR}/src/os/win/time_win.c")
else()
    target_sources(test_lsp_request_cancel PRIVATE "${PROJECT_SOURCE_DIR}/src/os/unix/thread_unix.c"
        "${PROJECT_SOURCE_DIR}/src/os/unix/time_unix.c")
endif()
target_include_directories(test_lsp_request_cancel PRIVATE "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/src/base" "${PROJECT_SOURCE_DIR}/include")
target_compile_features(test_lsp_request_cancel PRIVATE c_std_11)
if(MSVC)
    target_compile_options(test_lsp_request_cancel PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_lsp_request_cancel PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(test_lsp_request_cancel PRIVATE pthread)
endif()
add_test(NAME test_lsp_request_cancel COMMAND test_lsp_request_cancel)
set_tests_properties(test_lsp_request_cancel PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE LABELS "unit;lsp;ownership")
