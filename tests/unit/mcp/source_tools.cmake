add_executable(test_mcp_source_tools ${CMAKE_CURRENT_LIST_DIR}/test_mcp_source_tools.c)
target_link_libraries(test_mcp_source_tools PRIVATE xray_mcp_source_tools)
add_test(NAME test_mcp_source_tools COMMAND test_mcp_source_tools)
set_tests_properties(test_mcp_source_tools PROPERTIES LABELS "unit;mcp;source" TIMEOUT 60)
add_test(NAME test_mcp_source_tools_resources COMMAND test_mcp_source_tools --resources)
set_tests_properties(test_mcp_source_tools_resources PROPERTIES
    LABELS "unit;mcp;source;faults;resources" TIMEOUT 120 COST 1 RUN_SERIAL TRUE)
