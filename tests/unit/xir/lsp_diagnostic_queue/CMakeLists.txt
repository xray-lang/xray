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
