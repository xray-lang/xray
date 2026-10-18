if(TARGET test_lsp_rename_task)
    return()
endif()
include("${CMAKE_CURRENT_LIST_DIR}/../lsp_source_task/CMakeLists.txt")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
source_task_target(test_lsp_rename_task "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_navigation.c")
source_task_target(test_lsp_rename_task_before "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_navigation.c"
    "${CMAKE_CURRENT_LIST_DIR}/before_task_owner.c")
set(rename_dir "${CMAKE_CURRENT_BINARY_DIR}/lsp-rename-task")
add_custom_command(OUTPUT "${rename_dir}/rename_handlers.inc.c"
    COMMAND "${Python3_EXECUTABLE}" -X utf8 "${CMAKE_CURRENT_LIST_DIR}/extract_handlers.py"
        "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_server.c"
        "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_handlers_textdoc.c"
        "${rename_dir}/rename_handlers.inc.c"
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/extract_handlers.py" "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_server.c"
        "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_handlers_textdoc.c" VERBATIM)
set_source_files_properties("${rename_dir}/rename_handlers.inc.c" PROPERTIES HEADER_FILE_ONLY TRUE)
foreach(target test_lsp_rename_task test_lsp_rename_task_before)
    target_sources(${target} PRIVATE "${rename_dir}/rename_handlers.inc.c")
    target_compile_definitions(${target} PRIVATE XLSP_RENAME_TASK=1)
    target_include_directories(${target} PRIVATE "${rename_dir}")
endforeach()
foreach(mode normal resources counterfactual)
    if(mode STREQUAL "counterfactual")
        set(image test_lsp_rename_task_before)
    else()
        set(image test_lsp_rename_task)
    endif()
    add_test(NAME test_lsp_rename_task_${mode} COMMAND "${Python3_EXECUTABLE}" -X utf8
        "${CMAKE_CURRENT_LIST_DIR}/test_rename_task.py" "$<TARGET_FILE:${image}>" "${PROJECT_SOURCE_DIR}/stdlib" --${mode})
    set_tests_properties(test_lsp_rename_task_${mode} PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE LABELS "unit;lsp;source;ownership;allocation")
endforeach()
set_tests_properties(test_lsp_rename_task_resources PROPERTIES PROCESSORS 8 COST 290)
