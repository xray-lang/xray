# Knowledge lookup has no Source, analyzer or execution dependency.
add_executable(test_mcp_knowledge
    ${CMAKE_CURRENT_LIST_DIR}/test_mcp_knowledge.c
    ${CMAKE_SOURCE_DIR}/src/app/mcp/xmcp_knowledge.c
    ${CMAKE_SOURCE_DIR}/src/app/mcp/xmcp_knowledge_generated.c
)
target_include_directories(test_mcp_knowledge PRIVATE
    ${CMAKE_SOURCE_DIR}/src/app/mcp
    ${CMAKE_SOURCE_DIR}/src/base
)
add_test(NAME test_mcp_knowledge COMMAND test_mcp_knowledge)
set_tests_properties(test_mcp_knowledge PROPERTIES LABELS "unit;mcp;knowledge")
