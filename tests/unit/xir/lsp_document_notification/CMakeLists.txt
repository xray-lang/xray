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
