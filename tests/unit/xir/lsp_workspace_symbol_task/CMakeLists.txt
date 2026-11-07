if(TARGET test_lsp_workspace_symbol_task)
    return()
endif()
include("${CMAKE_CURRENT_LIST_DIR}/../lsp_source_task/CMakeLists.txt")
include("${CMAKE_CURRENT_LIST_DIR}/../lsp_source_index_store/CMakeLists.txt")

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(workspace_dir "${CMAKE_CURRENT_BINARY_DIR}/lsp-workspace-symbol-task")
foreach(kind IN ITEMS current)
    if(kind STREQUAL "before")
        set(extra "${CMAKE_CURRENT_LIST_DIR}/before_extra.c")
        set(textdoc "${CMAKE_CURRENT_LIST_DIR}/before_extra.c")
        set(output before_handlers.inc.c)
        set(flag --before)
    else()
        set(extra "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_extra_handlers.c")
        set(textdoc "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_extra_handlers.c")
        set(output workspace_handlers.inc.c)
        set(flag)
    endif()
    add_custom_command(OUTPUT "${workspace_dir}/${output}"
        COMMAND "${Python3_EXECUTABLE}" -X utf8 "${CMAKE_CURRENT_LIST_DIR}/extract_handlers.py"
            "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_server.c" "${workspace_dir}/${output}" "${extra}" "${textdoc}" ${flag}
        DEPENDS "${CMAKE_CURRENT_LIST_DIR}/extract_handlers.py" "${extra}" "${textdoc}" "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_server.c" VERBATIM)
    set_source_files_properties("${workspace_dir}/${output}" PROPERTIES HEADER_FILE_ONLY TRUE)

endforeach()

add_test(NAME test_lsp_source_index_store_normal COMMAND test_lsp_source_index_store --normal)
set_tests_properties(test_lsp_source_index_store_normal PROPERTIES TIMEOUT 30 LABELS "unit;lsp;ownership;metadata;allocation")
