if(TARGET test_lsp_rename_task)
    return()
endif()
include("${CMAKE_CURRENT_LIST_DIR}/../lsp_source_task/CMakeLists.txt")
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(rename_dir "${CMAKE_CURRENT_BINARY_DIR}/lsp-rename-task")
add_custom_command(OUTPUT "${rename_dir}/rename_handlers.inc.c"
    COMMAND "${Python3_EXECUTABLE}" -X utf8 "${CMAKE_CURRENT_LIST_DIR}/extract_handlers.py"
        "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_server.c"
        "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_handlers_textdoc.c"
        "${rename_dir}/rename_handlers.inc.c"
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/extract_handlers.py" "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_server.c"
        "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_handlers_textdoc.c" VERBATIM)
set_source_files_properties("${rename_dir}/rename_handlers.inc.c" PROPERTIES HEADER_FILE_ONLY TRUE)

foreach(mode normal resources counterfactual)
    if(mode STREQUAL "counterfactual")
        set(image test_lsp_rename_task_before)
    else()
        set(image test_lsp_rename_task)
    endif()

endforeach()
