# Source tools consume the single checked semantic owner without a VM isolate.
include_guard(GLOBAL)
add_library(xray_mcp_source_tools STATIC
    ${CMAKE_CURRENT_LIST_DIR}/xmcp_source.c
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
target_link_libraries(xray_mcp_source_tools PUBLIC xray_xir_source xray_frontend_format)
target_include_directories(xray_mcp_source_tools PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_features(xray_mcp_source_tools PUBLIC c_std_11)
if(MSVC)
    target_compile_options(xray_mcp_source_tools PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(xray_mcp_source_tools PRIVATE -Wall -Wextra -Werror)
endif()
