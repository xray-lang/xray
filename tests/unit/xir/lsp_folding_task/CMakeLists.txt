if(TARGET test_lsp_folding_task)
    return()
endif()
include("${CMAKE_CURRENT_LIST_DIR}/../lsp_source_task/CMakeLists.txt")
source_task_target(test_lsp_folding_task "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_navigation.c")
target_sources(test_lsp_folding_task PRIVATE "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_folding.c"
    "${CMAKE_CURRENT_LIST_DIR}/before_owner.c")
target_compile_definitions(test_lsp_folding_task PRIVATE XLSP_FOLDING_TASK=1)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(mode_dir "${CMAKE_CURRENT_BINARY_DIR}/lsp-folding-task")
add_custom_command(OUTPUT "${mode_dir}/folding_server_mode.inc.c" "${mode_dir}/folding-mode-source.json"
    COMMAND "${Python3_EXECUTABLE}" -X utf8 "${CMAKE_CURRENT_LIST_DIR}/extract_mode.py"
        "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_server.c" "${mode_dir}/folding_server_mode.inc.c"
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/extract_mode.py" "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_server.c" VERBATIM)
target_sources(test_lsp_folding_task PRIVATE "${mode_dir}/folding_server_mode.inc.c")
set_source_files_properties("${mode_dir}/folding_server_mode.inc.c" PROPERTIES HEADER_FILE_ONLY TRUE)
target_include_directories(test_lsp_folding_task PRIVATE "${mode_dir}")
foreach(mode normal resources counterfactual)
    add_test(NAME test_lsp_folding_task_${mode} COMMAND "${Python3_EXECUTABLE}" -X utf8
        "${CMAKE_CURRENT_LIST_DIR}/test_folding.py" "$<TARGET_FILE:test_lsp_folding_task>" "${PROJECT_SOURCE_DIR}/stdlib" --${mode})
    set_tests_properties(test_lsp_folding_task_${mode} PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE LABELS "unit;lsp;source;ownership;allocation")
endforeach()
set_tests_properties(test_lsp_folding_task_resources PROPERTIES PROCESSORS 8 COST 290)
