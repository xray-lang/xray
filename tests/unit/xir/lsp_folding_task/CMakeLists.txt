if(TARGET test_lsp_folding_task)
    return()
endif()
include("${CMAKE_CURRENT_LIST_DIR}/../lsp_source_task/CMakeLists.txt")

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(mode_dir "${CMAKE_CURRENT_BINARY_DIR}/lsp-folding-task")
add_custom_command(OUTPUT "${mode_dir}/folding_server_mode.inc.c" "${mode_dir}/folding-mode-source.json"
    COMMAND "${Python3_EXECUTABLE}" -X utf8 "${CMAKE_CURRENT_LIST_DIR}/extract_mode.py"
        "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_server.c" "${mode_dir}/folding_server_mode.inc.c"
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/extract_mode.py" "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_server.c" VERBATIM)

set_source_files_properties("${mode_dir}/folding_server_mode.inc.c" PROPERTIES HEADER_FILE_ONLY TRUE)
