if(TARGET test_lsp_completion_resolve_errors)
    return()
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(error_fixture "${CMAKE_CURRENT_LIST_DIR}")
set(error_generated "${CMAKE_CURRENT_BINARY_DIR}/lsp-completion-resolve-errors")
foreach(kind resolve finish send)
    if(kind STREQUAL "resolve")
        set(input "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_handlers_textdoc.c")
        set(functions xlsp_handle_td_completion_resolve)
    elseif(kind STREQUAL "finish")
        set(input "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_server.c")
        set(functions finish_request_handler)
    else()
        set(input "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_server.c")
        set(functions send_protocol_message send_response send_error)
    endif()
    add_custom_command(OUTPUT "${error_generated}/${kind}.inc.c"
        COMMAND "${Python3_EXECUTABLE}" -X utf8 "${error_fixture}/extract.py"
            "${input}" "${error_generated}/${kind}.inc.c" ${functions}
        DEPENDS "${input}" "${error_fixture}/extract.py" VERBATIM)
    set_source_files_properties("${error_generated}/${kind}.inc.c" PROPERTIES HEADER_FILE_ONLY TRUE)
endforeach()
foreach(target test_lsp_completion_resolve_errors test_lsp_completion_resolve_errors_before)
    add_executable(${target} "${error_fixture}/test_wire.c" "${error_generated}/resolve.inc.c"
        "${error_generated}/finish.inc.c" "${error_generated}/send.inc.c"
        "${PROJECT_SOURCE_DIR}/tests/unit/xir/lsp_json_clone/json_owner.c"
        "${PROJECT_SOURCE_DIR}/tests/unit/xir/lsp_json_clone/test_allocator.c"
        "${PROJECT_SOURCE_DIR}/src/base/xutf8.c" "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_framing.c")
    target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/src/base"
        "${PROJECT_SOURCE_DIR}/src/app/lsp" "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/tests/unit/xir/lsp_json_clone" "${error_generated}" "${error_fixture}")
    target_compile_features(${target} PRIVATE c_std_11)
    if(WIN32)
        target_sources(${target} PRIVATE "${PROJECT_SOURCE_DIR}/src/os/win/pipe_win.c"
            "${PROJECT_SOURCE_DIR}/src/os/win/fd_win.c" "${PROJECT_SOURCE_DIR}/src/os/win/time_win.c")
    else()
        target_sources(${target} PRIVATE "${PROJECT_SOURCE_DIR}/src/os/unix/pipe_unix.c"
            "${PROJECT_SOURCE_DIR}/src/os/unix/fd_unix.c" "${PROJECT_SOURCE_DIR}/src/os/unix/time_unix.c")
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
target_compile_definitions(test_lsp_completion_resolve_errors_before PRIVATE ERRORS_BEFORE=1)
foreach(mode normal resources before)
    if(mode STREQUAL "before")
        set(image test_lsp_completion_resolve_errors_before)
    else()
        set(image test_lsp_completion_resolve_errors)
    endif()
    add_test(NAME test_lsp_completion_resolve_errors_${mode} COMMAND "${Python3_EXECUTABLE}" -X utf8
        "${error_fixture}/test_wire.py" "$<TARGET_FILE:${image}>" --${mode})
    set_tests_properties(test_lsp_completion_resolve_errors_${mode} PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE
        LABELS "unit;lsp;json;protocol;ownership;allocation")
endforeach()
