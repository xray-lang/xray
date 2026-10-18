if(TARGET test_lsp_source_completion)
    return()
endif()
add_executable(test_lsp_pending_requests "${CMAKE_CURRENT_LIST_DIR}/test_pending.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_pending_requests.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_protocol_message.c"
    "${PROJECT_SOURCE_DIR}/src/base/xjson.c" "${PROJECT_SOURCE_DIR}/src/base/xutf8.c")
add_executable(test_lsp_source_completion "${CMAKE_CURRENT_LIST_DIR}/test_completion.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_source_completion.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_pending_requests.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_protocol_message.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_transport.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_framing.c"
    "${PROJECT_SOURCE_DIR}/src/base/xutf8.c")
if(WIN32)
    target_sources(test_lsp_source_completion PRIVATE "${PROJECT_SOURCE_DIR}/src/os/win/fd_win.c"
        "${PROJECT_SOURCE_DIR}/src/os/win/pipe_win.c" "${PROJECT_SOURCE_DIR}/src/os/win/time_win.c")
else()
    target_sources(test_lsp_source_completion PRIVATE "${PROJECT_SOURCE_DIR}/src/os/unix/fd_unix.c"
        "${PROJECT_SOURCE_DIR}/src/os/unix/pipe_unix.c" "${PROJECT_SOURCE_DIR}/src/os/unix/time_unix.c")
endif()
foreach(target IN ITEMS test_lsp_pending_requests test_lsp_source_completion)
    target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/src/base"
        "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/tests/unit/xir")
    target_compile_features(${target} PRIVATE c_std_11)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME test_lsp_pending_requests COMMAND test_lsp_pending_requests)
set_tests_properties(test_lsp_pending_requests PROPERTIES TIMEOUT 30 LABELS "unit;lsp;ownership")
add_test(NAME test_lsp_source_completion COMMAND "${Python3_EXECUTABLE}" -X utf8
    "${CMAKE_CURRENT_LIST_DIR}/test_completion.py" "$<TARGET_FILE:test_lsp_source_completion>")
set_tests_properties(test_lsp_source_completion PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE LABELS "unit;lsp;ownership;allocation")
