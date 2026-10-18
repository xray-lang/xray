# Observe unchanged canonical runtime sources; only the MCP caller arms faults.
get_target_property(_mcp_runtime_sources xray_xir_scalar SOURCES)
get_target_property(_mcp_runtime_source_dir xray_xir_scalar SOURCE_DIR)
get_target_property(_mcp_runtime_links xray_xir_scalar LINK_LIBRARIES)
set(_mcp_runtime_absolute_sources)
foreach(_source IN LISTS _mcp_runtime_sources)
    if(IS_ABSOLUTE "${_source}")
        list(APPEND _mcp_runtime_absolute_sources "${_source}")
    else()
        list(APPEND _mcp_runtime_absolute_sources "${_mcp_runtime_source_dir}/${_source}")
    endif()
endforeach()
add_library(xray_mcp_runtime_observed STATIC ${_mcp_runtime_absolute_sources})
target_include_directories(xray_mcp_runtime_observed PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(xray_mcp_runtime_observed PUBLIC ${_mcp_runtime_links})
add_library(xray_mcp_run_observed OBJECT ${CMAKE_SOURCE_DIR}/src/app/mcp/xmcp_tools_run.c)
target_include_directories(xray_mcp_run_observed PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(MSVC)
    target_compile_options(xray_mcp_runtime_observed PRIVATE /W4 /WX /utf-8
        /FI${CMAKE_CURRENT_LIST_DIR}/mcp_runtime_allocations.h)
    target_compile_options(xray_mcp_run_observed PRIVATE /W4 /WX /utf-8
        /FI${CMAKE_CURRENT_LIST_DIR}/mcp_runtime_boundary.h)
else()
    target_compile_options(xray_mcp_runtime_observed PRIVATE -Wall -Wextra -Werror
        -include ${CMAKE_CURRENT_LIST_DIR}/mcp_runtime_allocations.h)
    target_compile_options(xray_mcp_run_observed PRIVATE -Wall -Wextra -Werror
        -include ${CMAKE_CURRENT_LIST_DIR}/mcp_runtime_boundary.h)
endif()
add_executable(test_mcp_run_runtime ${CMAKE_CURRENT_LIST_DIR}/test_mcp_run_runtime.c
    $<TARGET_OBJECTS:xray_mcp_run_observed>)
target_link_libraries(test_mcp_run_runtime PRIVATE xray_mcp_runtime_observed xray_mcp)
add_test(NAME test_mcp_run_runtime COMMAND test_mcp_run_runtime)
set_tests_properties(test_mcp_run_runtime PROPERTIES
    LABELS "unit;mcp;execution;faults;ownership;task" TIMEOUT 120 COST 2 RUN_SERIAL TRUE
    ENVIRONMENT "XRAY_STDLIB_PATH=${CMAKE_SOURCE_DIR}/stdlib")

add_test(NAME test_mcp_run_runtime_normal COMMAND test_mcp_run_runtime --normal)
set_tests_properties(test_mcp_run_runtime_normal PROPERTIES
    LABELS "unit;mcp;execution;ownership;task" TIMEOUT 60
    ENVIRONMENT "XRAY_STDLIB_PATH=${CMAKE_SOURCE_DIR}/stdlib")
