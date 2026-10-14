add_executable(test_mcp_source_tools ${CMAKE_CURRENT_LIST_DIR}/test_mcp_source_tools.c)
target_link_libraries(test_mcp_source_tools PRIVATE xray_mcp)
add_test(NAME test_mcp_source_tools COMMAND test_mcp_source_tools)
set_tests_properties(test_mcp_source_tools PROPERTIES LABELS "unit;mcp;source" TIMEOUT 60)
add_test(NAME test_mcp_source_tools_resources COMMAND test_mcp_source_tools --resources)
set_tests_properties(test_mcp_source_tools_resources PROPERTIES
    LABELS "unit;mcp;source;faults;resources" TIMEOUT 120 COST 1 RUN_SERIAL TRUE)
add_executable(test_mcp_run_tools ${CMAKE_CURRENT_LIST_DIR}/test_mcp_run_tools.c)
target_link_libraries(test_mcp_run_tools PRIVATE xray_mcp)
add_test(NAME test_mcp_run_tools COMMAND test_mcp_run_tools)
set_tests_properties(test_mcp_run_tools PROPERTIES LABELS "unit;mcp;execution" TIMEOUT 60)
add_test(NAME test_mcp_run_tools_resources COMMAND test_mcp_run_tools --resources)
set_tests_properties(test_mcp_run_tools_resources PROPERTIES
    LABELS "unit;mcp;execution;faults;resources" TIMEOUT 120 COST 2 RUN_SERIAL TRUE)

add_executable(test_mcp_service ${CMAKE_CURRENT_LIST_DIR}/test_mcp_service.c)
target_link_libraries(test_mcp_service PRIVATE xray_mcp)
add_test(NAME test_mcp_service_transcript
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/mcp/test_protocol_transcript.py
        --xray $<TARGET_FILE:test_mcp_service>)
set_tests_properties(test_mcp_service_transcript PROPERTIES LABELS "unit;mcp;process" TIMEOUT 120)
