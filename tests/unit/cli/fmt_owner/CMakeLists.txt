# Included by the real repository build; no legacy core dependency.
if(NOT WIN32)
    return()
endif()
get_filename_component(FMT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(fmt_cli_sources "${FMT_ROOT}/src/app/cli/xcmd_fmt.c"
    "${FMT_ROOT}/src/app/cli/xcli_spec.c" "${FMT_ROOT}/src/app/cli/xcli_parser.c"
    "${FMT_ROOT}/src/app/cli/xcli_diag.c" "${FMT_ROOT}/src/app/cli/xcli_output.c"
    "${FMT_ROOT}/src/app/cli/xcli_help.c" "${FMT_ROOT}/src/os/win/fd_win.c"
    "${FMT_ROOT}/src/os/win/time_win.c")
add_executable(fmt_owner_driver "${CMAKE_CURRENT_LIST_DIR}/fmt_owner_driver.c" ${fmt_cli_sources})
target_link_libraries(fmt_owner_driver PRIVATE xray_frontend_format xray_cli_source xray_xir_publication)
target_include_directories(fmt_owner_driver PRIVATE "${FMT_ROOT}/src" "${FMT_ROOT}/include" "${CMAKE_BINARY_DIR}/generated")
if(MSVC)
    target_compile_definitions(fmt_owner_driver PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_options(fmt_owner_driver PRIVATE /W4 /WX /utf-8)
endif()
add_library(format_protocol_compile_probe OBJECT EXCLUDE_FROM_ALL
    "${FMT_ROOT}/src/app/lsp/xlsp_format.c" "${FMT_ROOT}/src/app/lsp/xlsp_handlers_textdoc.c"
    "${FMT_ROOT}/src/app/lsp/xlsp_server.c" "${FMT_ROOT}/src/app/mcp/xmcp_tools_lang.c")
target_link_libraries(format_protocol_compile_probe PRIVATE xray_frontend_format)
target_include_directories(format_protocol_compile_probe PRIVATE ${XRAY_COMMON_INCLUDES})
if(MSVC)
    target_compile_options(format_protocol_compile_probe PRIVATE /W4 /WX /utf-8)
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME fmt_owner_cases COMMAND "${Python3_EXECUTABLE}" -X utf8 "${CMAKE_CURRENT_LIST_DIR}/fmt_owner_cases.py"
    --driver "$<TARGET_FILE:fmt_owner_driver>" --output "${CMAKE_CURRENT_BINARY_DIR}/fmt-owner-cases")
set_tests_properties(fmt_owner_cases PROPERTIES TIMEOUT 90)

set(protocol_selection "${CMAKE_CURRENT_BINARY_DIR}/protocol-selected.inc")
add_custom_command(OUTPUT "${protocol_selection}"
    COMMAND "${Python3_EXECUTABLE}" -X utf8 "${CMAKE_CURRENT_LIST_DIR}/extract_protocol.py" --root "${FMT_ROOT}" --output "${protocol_selection}"
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/extract_protocol.py"
      "${FMT_ROOT}/src/app/lsp/xlsp_server.c" "${FMT_ROOT}/src/app/lsp/xlsp_handlers_textdoc.c"
      "${FMT_ROOT}/src/app/lsp/xlsp_transport.c" "${FMT_ROOT}/src/app/mcp/xmcp_tools_lang.c"
      "${FMT_ROOT}/src/app/mcp/xmcp_tools.c" VERBATIM)
add_executable(test_protocol_format "${CMAKE_CURRENT_LIST_DIR}/test_protocol_format.c"
    "${protocol_selection}" "${FMT_ROOT}/src/app/lsp/xlsp_format.c" "${FMT_ROOT}/src/base/xjson.c" "${FMT_ROOT}/src/base/xframing.c")
target_link_libraries(test_protocol_format PRIVATE xray_frontend_format)
target_include_directories(test_protocol_format PRIVATE "${FMT_ROOT}/src" "${FMT_ROOT}/include" "${CMAKE_CURRENT_BINARY_DIR}")
if(MSVC)
    target_compile_options(test_protocol_format PRIVATE /W4 /WX /utf-8)
endif()
add_test(NAME test_protocol_format COMMAND test_protocol_format)

set(fmt_fault_sources ${fmt_cli_sources})
list(REMOVE_ITEM fmt_fault_sources "${FMT_ROOT}/src/app/cli/xcmd_fmt.c")
add_executable(fmt_owner_faults "${CMAKE_CURRENT_LIST_DIR}/fmt_owner_driver.c" "${CMAKE_CURRENT_LIST_DIR}/test_fmt_dir_io.c" "${CMAKE_CURRENT_LIST_DIR}/test_fmt_command_io.c" ${fmt_fault_sources})
target_compile_definitions(fmt_owner_faults PRIVATE FMT_INJECTED=1 WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(fmt_owner_faults PRIVATE /W4 /WX /utf-8)
target_include_directories(fmt_owner_faults PRIVATE "${FMT_ROOT}/src" "${FMT_ROOT}/include" "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(fmt_owner_faults PRIVATE xray_frontend_format xray_cli_source xray_xir_publication)

add_test(NAME fmt_owner_faults COMMAND "${Python3_EXECUTABLE}" -X utf8 "${CMAKE_CURRENT_LIST_DIR}/fmt_owner_faults.py"
    --driver "$<TARGET_FILE:fmt_owner_faults>" --output "${CMAKE_CURRENT_BINARY_DIR}/fmt-owner-faults")
set_tests_properties(fmt_owner_faults PROPERTIES TIMEOUT 180 COST 20)
