if(TARGET test_lsp_enum_payload)
    return()
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(enum_payload_generated "${CMAKE_CURRENT_BINARY_DIR}/lsp_enum_payload_generated")
set(enum_payload_fragments "${enum_payload_generated}/enum_strategy_actual.inc.c"
    "${enum_payload_generated}/enum_response_actual.inc.c")
add_custom_command(OUTPUT ${enum_payload_fragments}
    COMMAND "${Python3_EXECUTABLE}" -X utf8 "${CMAKE_CURRENT_LIST_DIR}/extract_actual.py"
        "${PROJECT_SOURCE_DIR}" "${enum_payload_generated}"
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/extract_actual.py"
        "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_completion.c"
        "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_handlers_textdoc.c"
        "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_document_position.c"
    VERBATIM)
set_source_files_properties(${enum_payload_fragments} PROPERTIES HEADER_FILE_ONLY TRUE)
add_executable(test_lsp_enum_payload "${CMAKE_CURRENT_LIST_DIR}/test_enum_payload.c" ${enum_payload_fragments})
target_link_libraries(test_lsp_enum_payload PRIVATE xray_lsp_source_syntax)
target_include_directories(test_lsp_enum_payload PRIVATE "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/src/base" "${PROJECT_SOURCE_DIR}/tests/unit/xir"
    "${PROJECT_SOURCE_DIR}/tests/unit/lsp" "${enum_payload_generated}")
target_compile_features(test_lsp_enum_payload PRIVATE c_std_11)
if(WIN32)
    target_link_libraries(test_lsp_enum_payload PRIVATE bcrypt ws2_32)
endif()
if(MSVC)
    target_compile_options(test_lsp_enum_payload PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_lsp_enum_payload PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_lsp_enum_payload COMMAND test_lsp_enum_payload)
set_tests_properties(test_lsp_enum_payload PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE
    LABELS "unit;lsp;source;ownership;allocation;metadata")
