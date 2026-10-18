if(TARGET test_lsp_document_owner)
    return()
endif()
add_executable(test_lsp_document_owner "${CMAKE_CURRENT_LIST_DIR}/test_document.c"
    "${CMAKE_CURRENT_LIST_DIR}/owner.c" "${CMAKE_CURRENT_LIST_DIR}/baseline.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_diagnostic_clear.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_protocol_message.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_transport.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_framing.c")
if(WIN32)
    target_sources(test_lsp_document_owner PRIVATE "${PROJECT_SOURCE_DIR}/src/os/win/time_win.c"
        "${PROJECT_SOURCE_DIR}/src/os/win/pipe_win.c" "${PROJECT_SOURCE_DIR}/src/os/win/fd_win.c")
    target_link_libraries(test_lsp_document_owner PRIVATE bcrypt)
else()
    target_sources(test_lsp_document_owner PRIVATE "${PROJECT_SOURCE_DIR}/src/os/unix/time_unix.c"
        "${PROJECT_SOURCE_DIR}/src/os/unix/pipe_unix.c" "${PROJECT_SOURCE_DIR}/src/os/unix/fd_unix.c")
endif()
target_link_libraries(test_lsp_document_owner PRIVATE xray_lsp_document_open)
target_include_directories(test_lsp_document_owner PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/src/base"
    "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/tests/unit/xir")
target_compile_features(test_lsp_document_owner PRIVATE c_std_11)
if(MSVC)
    target_compile_options(test_lsp_document_owner PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_lsp_document_owner PRIVATE -Wall -Wextra -Werror)
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME test_lsp_document_owner COMMAND "${Python3_EXECUTABLE}" -X utf8
    "${CMAKE_CURRENT_LIST_DIR}/test_document.py" "$<TARGET_FILE:test_lsp_document_owner>" "${PROJECT_SOURCE_DIR}/stdlib")
set_tests_properties(test_lsp_document_owner PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE LABELS "unit;lsp;source;ownership")
