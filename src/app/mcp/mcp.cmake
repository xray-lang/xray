# MCP consumes the single Source/Program/HostCall pipeline without a legacy isolate.
include_guard(GLOBAL)
add_library(xray_mcp STATIC
    ${CMAKE_CURRENT_LIST_DIR}/xmcp_server.c
    ${CMAKE_CURRENT_LIST_DIR}/xmcp_json.c
    ${CMAKE_CURRENT_LIST_DIR}/xmcp_jsonrpc.c
    ${CMAKE_CURRENT_LIST_DIR}/xmcp_protocol.c
    ${CMAKE_CURRENT_LIST_DIR}/xmcp_registry.c
    ${CMAKE_CURRENT_LIST_DIR}/xmcp_tools.c
    ${CMAKE_CURRENT_LIST_DIR}/xmcp_resources.c
    ${CMAKE_CURRENT_LIST_DIR}/xmcp_prompts.c
    ${CMAKE_CURRENT_LIST_DIR}/xmcp_source.c
    ${CMAKE_CURRENT_LIST_DIR}/xmcp_run_source.c
    ${CMAKE_CURRENT_LIST_DIR}/xmcp_tools_run.c
    ${CMAKE_CURRENT_LIST_DIR}/xmcp_tools_lang.c
    ${CMAKE_CURRENT_LIST_DIR}/xmcp_tools_source_query.c
    ${CMAKE_CURRENT_LIST_DIR}/xmcp_tools_kb.c
    ${CMAKE_CURRENT_LIST_DIR}/xmcp_tools_schema.c
    ${CMAKE_CURRENT_LIST_DIR}/xmcp_result.c
    ${CMAKE_CURRENT_LIST_DIR}/xmcp_notifications.c
    ${CMAKE_CURRENT_LIST_DIR}/xmcp_transport_stdio.c
    ${CMAKE_CURRENT_LIST_DIR}/xmcp_knowledge.c
    ${CMAKE_CURRENT_LIST_DIR}/xmcp_knowledge_generated.c
    ${CMAKE_SOURCE_DIR}/src/base/xjson.c
)
target_link_libraries(xray_mcp PUBLIC xray_source_compile xray_xir_runtime_host xray_frontend_format)
target_include_directories(xray_mcp PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_features(xray_mcp PUBLIC c_std_11)
if(MSVC)
    target_compile_options(xray_mcp PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(xray_mcp PRIVATE -Wall -Wextra -Werror)
endif()

if(WIN32)
    target_sources(xray_mcp PRIVATE ${CMAKE_SOURCE_DIR}/src/os/win/fd_win.c)
else()
    target_sources(xray_mcp PRIVATE ${CMAKE_SOURCE_DIR}/src/os/unix/fd_unix.c)
endif()
