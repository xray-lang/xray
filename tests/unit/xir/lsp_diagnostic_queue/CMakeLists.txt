if(TARGET test_lsp_diagnostic_queue)
    return()
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(queue_dir "${CMAKE_CURRENT_BINARY_DIR}/lsp-diagnostic-queue")
add_custom_command(OUTPUT "${queue_dir}/notification_handlers.inc.c"
    COMMAND "${Python3_EXECUTABLE}" -X utf8 "${PROJECT_SOURCE_DIR}/tests/unit/xir/lsp_document_notification/extract_handlers.py"
        "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_handlers_textdoc.c" "${queue_dir}/notification_handlers.inc.c"
    DEPENDS "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_handlers_textdoc.c"
        "${PROJECT_SOURCE_DIR}/tests/unit/xir/lsp_document_notification/extract_handlers.py")
foreach(kind current before)
    if(kind STREQUAL "current")
        set(input "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_document_owner.c")
        set(output "current_schedule.inc.c")
        set(extra)
    else()
        set(input "${CMAKE_CURRENT_LIST_DIR}/before_owner.c")
        set(output "before_schedule.inc.c")
        set(extra --predecessor)
    endif()
    add_custom_command(OUTPUT "${queue_dir}/${output}"
        COMMAND "${Python3_EXECUTABLE}" -X utf8 "${CMAKE_CURRENT_LIST_DIR}/extract_schedule.py"
            "${input}" "${queue_dir}/${output}" ${extra}
        DEPENDS "${input}" "${CMAKE_CURRENT_LIST_DIR}/extract_schedule.py")
endforeach()
set_source_files_properties("${queue_dir}/notification_handlers.inc.c" "${queue_dir}/current_schedule.inc.c"
    "${queue_dir}/before_schedule.inc.c" PROPERTIES HEADER_FILE_ONLY TRUE)
add_executable(test_lsp_diagnostic_queue "${CMAKE_CURRENT_LIST_DIR}/test_queue.c" "${CMAKE_CURRENT_LIST_DIR}/owner.c"
    "${queue_dir}/notification_handlers.inc.c" "${queue_dir}/current_schedule.inc.c" "${queue_dir}/before_schedule.inc.c"
    "${PROJECT_SOURCE_DIR}/tests/unit/xir/lsp_document_owner/baseline.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_document_notification.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_diagnostic_clear.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_protocol_message.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_transport.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_framing.c")
if(WIN32)
    target_sources(test_lsp_diagnostic_queue PRIVATE "${PROJECT_SOURCE_DIR}/src/os/win/time_win.c"
        "${PROJECT_SOURCE_DIR}/src/os/win/pipe_win.c" "${PROJECT_SOURCE_DIR}/src/os/win/fd_win.c")
    target_link_libraries(test_lsp_diagnostic_queue PRIVATE bcrypt)
else()
    target_sources(test_lsp_diagnostic_queue PRIVATE "${PROJECT_SOURCE_DIR}/src/os/unix/time_unix.c"
        "${PROJECT_SOURCE_DIR}/src/os/unix/pipe_unix.c" "${PROJECT_SOURCE_DIR}/src/os/unix/fd_unix.c")
endif()
target_link_libraries(test_lsp_diagnostic_queue PRIVATE xray_lsp_document_open)
target_include_directories(test_lsp_diagnostic_queue PRIVATE "${queue_dir}" "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/src/base" "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/tests/unit/xir")
target_compile_features(test_lsp_diagnostic_queue PRIVATE c_std_11)
if(MSVC)
    target_compile_options(test_lsp_diagnostic_queue PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_lsp_diagnostic_queue PRIVATE -Wall -Wextra -Werror)
endif()
foreach(mode normal before-oom before-deadline resources)
    add_test(NAME test_lsp_diagnostic_queue_${mode} COMMAND "${Python3_EXECUTABLE}" -X utf8
        "${CMAKE_CURRENT_LIST_DIR}/test_queue.py" "$<TARGET_FILE:test_lsp_diagnostic_queue>" "${PROJECT_SOURCE_DIR}/stdlib" --${mode})
    set_tests_properties(test_lsp_diagnostic_queue_${mode} PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE
        LABELS "unit;lsp;protocol;source;ownership;allocation")
endforeach()
