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
get_target_property(_mcp_fault_sources xray_mcp SOURCES)
get_target_property(_mcp_fault_links xray_mcp LINK_LIBRARIES)
add_library(xray_mcp_faults STATIC ${_mcp_fault_sources})
target_link_libraries(xray_mcp_faults PUBLIC ${_mcp_fault_links})
target_include_directories(xray_mcp_faults PUBLIC ${CMAKE_SOURCE_DIR}/src)
if(MSVC)
    target_compile_options(xray_mcp_faults PRIVATE /W4 /WX /utf-8
        /FI${CMAKE_CURRENT_LIST_DIR}/mcp_json_allocations.h)
else()
    target_compile_options(xray_mcp_faults PRIVATE -Wall -Wextra -Werror
        -include ${CMAKE_CURRENT_LIST_DIR}/mcp_json_allocations.h)
endif()
add_executable(test_mcp_json_faults ${CMAKE_CURRENT_LIST_DIR}/test_mcp_json_faults.c)
target_link_libraries(test_mcp_json_faults PRIVATE xray_mcp_faults)
add_test(NAME test_mcp_json_faults COMMAND test_mcp_json_faults)
set_tests_properties(test_mcp_json_faults PROPERTIES LABELS "unit;mcp;faults;ownership" TIMEOUT 120 COST 10 RUN_SERIAL TRUE)

set_tests_properties(test_mcp_run_tools test_mcp_run_tools_resources test_mcp_json_faults
    test_mcp_service_transcript PROPERTIES ENVIRONMENT "XRAY_STDLIB_PATH=${CMAKE_SOURCE_DIR}/stdlib")

add_executable(test_mcp_publication_faults ${CMAKE_CURRENT_LIST_DIR}/test_mcp_publication_faults.c)
target_link_libraries(test_mcp_publication_faults PRIVATE xray_mcp_faults)
add_test(NAME test_mcp_publication_faults COMMAND test_mcp_publication_faults)
set_tests_properties(test_mcp_publication_faults PROPERTIES
    LABELS "unit;mcp;faults;ownership;process" TIMEOUT 120 COST 1 RUN_SERIAL TRUE)

add_executable(test_mcp_knowledge_faults ${CMAKE_CURRENT_LIST_DIR}/test_mcp_json_faults.c)
target_compile_definitions(test_mcp_knowledge_faults PRIVATE MCP_KNOWLEDGE_FAULTS=1)
target_link_libraries(test_mcp_knowledge_faults PRIVATE xray_mcp_faults)
add_test(NAME test_mcp_knowledge_faults COMMAND test_mcp_knowledge_faults)
set_tests_properties(test_mcp_knowledge_faults PROPERTIES
    LABELS "unit;mcp;faults;ownership;knowledge" TIMEOUT 120 COST 2 RUN_SERIAL TRUE)
