if(TARGET test_lsp_document_notification)
    return()
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(notification_dir "${CMAKE_CURRENT_BINARY_DIR}/lsp-document-notification")
foreach(kind current before)
    if(kind STREQUAL "current")
        set(input "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_handlers_textdoc.c")
        set(output "notification_handlers.inc.c")
        set(extra)
    else()
        set(input "${CMAKE_CURRENT_LIST_DIR}/before_textdoc.c")
        set(output "before_handlers.inc.c")
        set(extra --predecessor)
    endif()
    add_custom_command(OUTPUT "${notification_dir}/${output}"
        COMMAND "${Python3_EXECUTABLE}" -X utf8 "${CMAKE_CURRENT_LIST_DIR}/extract_handlers.py"
            "${input}" "${notification_dir}/${output}" ${extra}
        DEPENDS "${input}" "${CMAKE_CURRENT_LIST_DIR}/extract_handlers.py")
endforeach()
set_source_files_properties("${notification_dir}/notification_handlers.inc.c"
    "${notification_dir}/before_handlers.inc.c" PROPERTIES HEADER_FILE_ONLY TRUE)
add_executable(test_lsp_document_notification "${CMAKE_CURRENT_LIST_DIR}/test_notification.c"
    "${notification_dir}/notification_handlers.inc.c" "${notification_dir}/before_handlers.inc.c"
    "${PROJECT_SOURCE_DIR}/tests/unit/xir/lsp_document_owner/owner.c"
    "${PROJECT_SOURCE_DIR}/tests/unit/xir/lsp_document_owner/baseline.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_document_notification.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_diagnostic_clear.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_protocol_message.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_transport.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_framing.c")
if(WIN32)
    target_sources(test_lsp_document_notification PRIVATE "${PROJECT_SOURCE_DIR}/src/os/win/time_win.c"
        "${PROJECT_SOURCE_DIR}/src/os/win/pipe_win.c" "${PROJECT_SOURCE_DIR}/src/os/win/fd_win.c")
    target_link_libraries(test_lsp_document_notification PRIVATE bcrypt)
else()
    target_sources(test_lsp_document_notification PRIVATE "${PROJECT_SOURCE_DIR}/src/os/unix/time_unix.c"
        "${PROJECT_SOURCE_DIR}/src/os/unix/pipe_unix.c" "${PROJECT_SOURCE_DIR}/src/os/unix/fd_unix.c")
endif()
target_link_libraries(test_lsp_document_notification PRIVATE xray_lsp_document_open)
target_include_directories(test_lsp_document_notification PRIVATE "${notification_dir}" "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/src/base" "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/tests/unit/xir")
target_compile_features(test_lsp_document_notification PRIVATE c_std_11)
if(MSVC)
    target_compile_options(test_lsp_document_notification PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_lsp_document_notification PRIVATE -Wall -Wextra -Werror)
endif()
foreach(mode normal before-change before-close resources)
    add_test(NAME test_lsp_document_notification_${mode} COMMAND "${Python3_EXECUTABLE}" -X utf8
        "${CMAKE_CURRENT_LIST_DIR}/test_notification.py" "$<TARGET_FILE:test_lsp_document_notification>"
        "${PROJECT_SOURCE_DIR}/stdlib" --${mode})
    set_tests_properties(test_lsp_document_notification_${mode} PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE
        LABELS "unit;lsp;protocol;source;ownership")
endforeach()
set_tests_properties(test_lsp_document_notification_resources PROPERTIES TIMEOUT 300 COST 290 PROCESSORS 8
    LABELS "unit;lsp;protocol;source;ownership;allocation")
