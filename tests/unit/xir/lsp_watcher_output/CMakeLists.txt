if(TARGET test_lsp_watcher_output)
    return()
endif()
add_executable(test_lsp_watcher_output "${CMAKE_CURRENT_LIST_DIR}/test_output.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_framing.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_protocol_message.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_watcher_registration.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_transport.c"
    "${PROJECT_SOURCE_DIR}/src/base/xutf8.c")
if(WIN32)
    target_sources(test_lsp_watcher_output PRIVATE "${PROJECT_SOURCE_DIR}/src/os/win/fd_win.c"
        "${PROJECT_SOURCE_DIR}/src/os/win/pipe_win.c" "${PROJECT_SOURCE_DIR}/src/os/win/time_win.c")
else()
    target_sources(test_lsp_watcher_output PRIVATE "${PROJECT_SOURCE_DIR}/src/os/unix/fd_unix.c"
        "${PROJECT_SOURCE_DIR}/src/os/unix/pipe_unix.c" "${PROJECT_SOURCE_DIR}/src/os/unix/time_unix.c")
endif()
target_include_directories(test_lsp_watcher_output PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/src/base" "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/tests/unit/xir")
target_compile_features(test_lsp_watcher_output PRIVATE c_std_11)
if(MSVC)
    target_compile_options(test_lsp_watcher_output PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_lsp_watcher_output PRIVATE -Wall -Wextra -Werror)
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME test_lsp_watcher_output COMMAND "${Python3_EXECUTABLE}" -X utf8
    "${CMAKE_CURRENT_LIST_DIR}/test_output.py" "$<TARGET_FILE:test_lsp_watcher_output>")
set_tests_properties(test_lsp_watcher_output PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE LABELS "unit;lsp;ownership;allocation")
