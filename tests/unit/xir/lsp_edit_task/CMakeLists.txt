if(TARGET test_lsp_edit_task)
    return()
endif()
include("${CMAKE_CURRENT_LIST_DIR}/../lsp_source_task/CMakeLists.txt")
include("${CMAKE_CURRENT_LIST_DIR}/../lsp_format/CMakeLists.txt")

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(edit_dir "${CMAKE_CURRENT_BINARY_DIR}/lsp-edit-task")
foreach(kind IN ITEMS current)
    if(kind STREQUAL "before")
        set(extra "${CMAKE_CURRENT_LIST_DIR}/before_format.c")
        set(textdoc "${CMAKE_CURRENT_LIST_DIR}/before_on_type.c")
        set(output before_handlers.inc.c)
        set(flag --before)
    else()
        set(extra "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_format.c")
        set(textdoc "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_on_type.c")
        set(output edit_handlers.inc.c)
        set(flag)
    endif()
    add_custom_command(OUTPUT "${edit_dir}/${output}"
        COMMAND "${Python3_EXECUTABLE}" -X utf8 "${CMAKE_CURRENT_LIST_DIR}/extract_handlers.py"
            "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_server.c" "${edit_dir}/${output}" "${extra}" "${textdoc}" ${flag}
        DEPENDS "${CMAKE_CURRENT_LIST_DIR}/extract_handlers.py" "${extra}" "${textdoc}" "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_server.c" VERBATIM)
    set_source_files_properties("${edit_dir}/${output}" PROPERTIES HEADER_FILE_ONLY TRUE)

endforeach()

foreach(original test_lsp_format_owner test_lsp_source_on_type)
    add_test(NAME ${original}_normal COMMAND ${original} --normal)
    set_tests_properties(${original}_normal PROPERTIES TIMEOUT 30 LABELS "unit;lsp;ownership;metadata;allocation")
endforeach()
