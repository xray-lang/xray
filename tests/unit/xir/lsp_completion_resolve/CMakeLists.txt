if(TARGET test_lsp_completion_resolve)
    return()
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(resolve_generated "${CMAKE_CURRENT_BINARY_DIR}/lsp-completion-resolve")
set(resolve_fixture "${CMAKE_CURRENT_LIST_DIR}")
foreach(kind current producer)
    if(kind STREQUAL "current")
        set(resolve_input "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_handlers_textdoc.c")
        set(resolve_function xlsp_handle_td_completion_resolve)
    else()
        set(resolve_input "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_completion.c")
        set(resolve_function make_completion_item)
    endif()
    add_custom_command(OUTPUT "${resolve_generated}/${kind}.inc.c"
        COMMAND "${Python3_EXECUTABLE}" -X utf8 "${resolve_fixture}/extract.py"
            "${resolve_input}" "${resolve_generated}/${kind}.inc.c" "${resolve_function}"
        DEPENDS "${resolve_input}" "${resolve_fixture}/extract.py" VERBATIM)
endforeach()
configure_file("${resolve_fixture}/before_resolve.inc.c" "${resolve_generated}/before.inc.c" COPYONLY)
set_source_files_properties("${resolve_generated}/current.inc.c" "${resolve_generated}/producer.inc.c"
    PROPERTIES HEADER_FILE_ONLY TRUE)
foreach(target test_lsp_completion_resolve test_lsp_completion_resolve_before)
    add_executable(${target} "${resolve_fixture}/test_resolve.c"
        "${resolve_generated}/current.inc.c" "${resolve_generated}/producer.inc.c"
        "${PROJECT_SOURCE_DIR}/tests/unit/xir/lsp_json_clone/test_allocator.c"
        "${PROJECT_SOURCE_DIR}/tests/unit/xir/lsp_json_clone/json_owner.c"
        "${PROJECT_SOURCE_DIR}/src/base/xutf8.c")
    target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/src/base"
        "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/tests/unit/xir/lsp_json_clone" "${resolve_generated}")
    target_compile_features(${target} PRIVATE c_std_11)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
target_compile_definitions(test_lsp_completion_resolve_before PRIVATE RESOLVE_BEFORE=1)
add_test(NAME test_lsp_completion_resolve_normal COMMAND test_lsp_completion_resolve --normal)
add_test(NAME test_lsp_completion_resolve_resources COMMAND test_lsp_completion_resolve --full)
add_test(NAME test_lsp_completion_resolve_before COMMAND test_lsp_completion_resolve_before)
set_tests_properties(test_lsp_completion_resolve_normal test_lsp_completion_resolve_resources
    test_lsp_completion_resolve_before PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE LABELS "unit;lsp;json;ownership;allocation")
