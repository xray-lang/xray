# Runtime fixtures are copied into this build tree before any queued test runs.
set(XR_DAP_FIXTURE_DIR "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
file(MAKE_DIRECTORY "${XR_DAP_FIXTURE_DIR}")
file(GLOB XR_DAP_CURRENT_FIXTURES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.xr")
file(GLOB XR_DAP_ORACLE_FIXTURES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/tests/regression/dap/fixtures/current/*.xr")
list(APPEND XR_DAP_CURRENT_FIXTURES ${XR_DAP_ORACLE_FIXTURES})
foreach(XR_DAP_INPUT IN LISTS XR_DAP_CURRENT_FIXTURES)
    get_filename_component(XR_DAP_INPUT_NAME "${XR_DAP_INPUT}" NAME)
    configure_file("${XR_DAP_INPUT}" "${XR_DAP_FIXTURE_DIR}/${XR_DAP_INPUT_NAME}" COPYONLY)
endforeach()

if(WIN32)
    add_subdirectory(process_handoff)
endif()










set(XR_DAP_NATIVE_TASK_DIR "${CMAKE_CURRENT_BINARY_DIR}/native-task")







set(XR_DAP_NATIVE_SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/native-source")




add_executable(test_xir_cell_instruction_storage test_cell_instruction_storage.c)
target_link_libraries(test_xir_cell_instruction_storage PRIVATE xray_xir xray_xir_scalar xray_xir_source)
target_include_directories(test_xir_cell_instruction_storage PRIVATE ${XRAY_COMMON_INCLUDES})
if(MSVC)
    target_compile_options(test_xir_cell_instruction_storage PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_cell_instruction_storage PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_cell_instruction_storage COMMAND test_xir_cell_instruction_storage)
set_tests_properties(test_xir_cell_instruction_storage PROPERTIES TIMEOUT 120 LABELS "dap;source;ownership;compiler")

xray_stage_windows_asan_runtime("${CMAKE_CURRENT_BINARY_DIR}")
cmake_language(DEFER CALL xray_prepend_windows_asan_runtime_to_directory_tests)

add_executable(test_dap_json test_json.c ${PROJECT_SOURCE_DIR}/src/base/xjson.c ${PROJECT_SOURCE_DIR}/src/base/xutf8.c)
target_link_libraries(test_dap_json PRIVATE xray_dap_current)
if(MSVC)
    target_compile_options(test_dap_json PRIVATE /W4 /WX)
else()
    target_compile_options(test_dap_json PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_dap_json COMMAND test_dap_json)
add_test(NAME test_dap_json_resources COMMAND test_dap_json --resources)
set_tests_properties(test_dap_json test_dap_json_resources PROPERTIES TIMEOUT 120 LABELS "dap;json;resources")



































add_executable(test_dap_stdio_owner test_stdio_owner.c ${PROJECT_SOURCE_DIR}/src/base/xjson.c ${PROJECT_SOURCE_DIR}/src/base/xutf8.c)
target_link_libraries(test_dap_stdio_owner PRIVATE xray_dap_current)
if(MSVC)
    target_compile_options(test_dap_stdio_owner PRIVATE /W4 /WX)
else()
    target_compile_options(test_dap_stdio_owner PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_dap_stdio_owner COMMAND test_dap_stdio_owner)
add_test(NAME test_dap_stdio_resources COMMAND test_dap_stdio_owner --resources)
set_tests_properties(test_dap_stdio_owner test_dap_stdio_resources PROPERTIES TIMEOUT 120 LABELS "dap;stdio;resources;ownership")

if(XR_BUILD_DAP)
    set(XR_DAP_SCRIPT_DIR "${CMAKE_CURRENT_BINARY_DIR}/worker-scripts")
    file(MAKE_DIRECTORY "${XR_DAP_SCRIPT_DIR}")
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/test_stdio_client.py ${XR_DAP_SCRIPT_DIR}/test_stdio_client.py COPYONLY)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/stdio_win_debug.py ${XR_DAP_SCRIPT_DIR}/stdio_win_debug.py COPYONLY)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/test_stdio_handoff.py ${XR_DAP_SCRIPT_DIR}/test_stdio_handoff.py COPYONLY)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/test_stdio_worker.py ${XR_DAP_SCRIPT_DIR}/test_stdio_worker.py COPYONLY)
    if(WIN32)
        foreach(XR_DAP_HANDOFF_MODE death fault resources)
            set(XR_DAP_HANDOFF_ARGS --case postcommit_death)
            if(XR_DAP_HANDOFF_MODE STREQUAL "fault")
                set(XR_DAP_HANDOFF_ARGS --case query_reject)
            elseif(XR_DAP_HANDOFF_MODE STREQUAL "resources")
                set(XR_DAP_HANDOFF_ARGS --resources)
            endif()
            add_test(NAME dap_current_stdio_handoff_${XR_DAP_HANDOFF_MODE} COMMAND ${XRAY_PYTHON} ${XR_DAP_SCRIPT_DIR}/test_stdio_handoff.py
                --xray $<TARGET_FILE:xray> --paused-fixture ${XR_DAP_FIXTURE_DIR}/threads_fixture.xr
                --streams-fixture ${XR_DAP_FIXTURE_DIR}/worker_streams_fixture.xr ${XR_DAP_HANDOFF_ARGS}
                --transcript ${CMAKE_CURRENT_BINARY_DIR}/handoff-${XR_DAP_HANDOFF_MODE}.json)
            set_tests_properties(dap_current_stdio_handoff_${XR_DAP_HANDOFF_MODE} PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE
                ENVIRONMENT "PYTHONUTF8=1" LABELS "dap;stdio;handoff;process;ownership")
        endforeach()
        set_tests_properties(dap_current_stdio_handoff_resources PROPERTIES LABELS "dap;stdio;handoff;process;ownership;resources;fault-injection")
        foreach(XR_DAP_WORKER_MODE unstarted stopped running timer attach output-closed frontend-killed terminate output-normal streams unicode timer-killed)
            set(XR_DAP_WORKER_SOURCE threads_fixture.xr)
            if(XR_DAP_WORKER_MODE STREQUAL "unstarted")
                set(XR_DAP_WORKER_SOURCE wire_fixture.xr)
            elseif(XR_DAP_WORKER_MODE STREQUAL "streams")
                set(XR_DAP_WORKER_SOURCE worker_streams_fixture.xr)
            elseif(XR_DAP_WORKER_MODE MATCHES "^(timer|output-closed|output-normal|unicode|timer-killed)$")
                set(XR_DAP_WORKER_SOURCE worker_fixture.xr)
            endif()
            add_test(NAME dap_current_stdio_worker_${XR_DAP_WORKER_MODE} COMMAND ${XRAY_PYTHON} ${XR_DAP_SCRIPT_DIR}/test_stdio.py
                --xray $<TARGET_FILE:xray> --fixture ${XR_DAP_FIXTURE_DIR}/${XR_DAP_WORKER_SOURCE} --scenario worker-${XR_DAP_WORKER_MODE})
            set_tests_properties(dap_current_stdio_worker_${XR_DAP_WORKER_MODE} PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE
                ENVIRONMENT "PYTHONUTF8=1" LABELS "dap;stdio;worker;detach;cli;execution;ownership")
        endforeach()
    endif()
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/test_stdio.py ${XR_DAP_SCRIPT_DIR}/test_stdio.py COPYONLY)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/test_full_session.py ${XR_DAP_SCRIPT_DIR}/test_full_session.py COPYONLY)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/test_throw_sites.py ${XR_DAP_SCRIPT_DIR}/test_throw_sites.py COPYONLY)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/test_exception_filters.py ${XR_DAP_SCRIPT_DIR}/test_exception_filters.py COPYONLY)
    foreach(XR_DAP_FILTER_TRANSPORT stdio tcp)
        set(XR_DAP_FILTER_START launch)
        if(XR_DAP_FILTER_TRANSPORT STREQUAL "tcp")
            set(XR_DAP_FILTER_START attach)
        endif()
        add_test(NAME dap_current_${XR_DAP_FILTER_TRANSPORT}_exception_filters COMMAND ${XRAY_PYTHON} ${XR_DAP_SCRIPT_DIR}/test_stdio.py
            --xray $<TARGET_FILE:xray> --fixture ${XR_DAP_FIXTURE_DIR}/exception_flows.xr --transport ${XR_DAP_FILTER_TRANSPORT}
            --scenario exception-filters --start-request ${XR_DAP_FILTER_START} --transcript ${CMAKE_CURRENT_BINARY_DIR}/exception-filters-${XR_DAP_FILTER_TRANSPORT}.json)
        set_tests_properties(dap_current_${XR_DAP_FILTER_TRANSPORT}_exception_filters PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1 LABELS "dap;cli;source;execution;ownership;filters;task")
    endforeach()
    foreach(XR_DAP_FULL_TRANSPORT stdio tcp)
        set(XR_DAP_FULL_START launch)
        if(XR_DAP_FULL_TRANSPORT STREQUAL "tcp")
            set(XR_DAP_FULL_START attach)
        endif()
        add_test(NAME dap_current_full_${XR_DAP_FULL_TRANSPORT}_${XR_DAP_FULL_START} COMMAND ${XRAY_PYTHON} ${XR_DAP_SCRIPT_DIR}/test_stdio.py
            --xray $<TARGET_FILE:xray> --fixture ${XR_DAP_FIXTURE_DIR}/full_session.xr --transport ${XR_DAP_FULL_TRANSPORT}
            --scenario full-session --start-request ${XR_DAP_FULL_START} --transcript ${CMAKE_CURRENT_BINARY_DIR}/full-session-${XR_DAP_FULL_TRANSPORT}.json)
        set_tests_properties(dap_current_full_${XR_DAP_FULL_TRANSPORT}_${XR_DAP_FULL_START} PROPERTIES
            TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1 LABELS "dap;cli;source;execution;ownership;full-session")
    endforeach()
    foreach(XR_DAP_THROW_TRANSPORT stdio tcp)
        add_test(NAME dap_current_${XR_DAP_THROW_TRANSPORT}_throw_sites COMMAND ${XRAY_PYTHON} ${XR_DAP_SCRIPT_DIR}/test_stdio.py
            --xray $<TARGET_FILE:xray> --fixture ${XR_DAP_FIXTURE_DIR}/throw_sites.xr --transport ${XR_DAP_THROW_TRANSPORT}
            --scenario throw-sites --transcript ${CMAKE_CURRENT_BINARY_DIR}/throw-sites-${XR_DAP_THROW_TRANSPORT}.json)
        set_tests_properties(dap_current_${XR_DAP_THROW_TRANSPORT}_throw_sites PROPERTIES
            TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1 LABELS "dap;cli;source;ownership;exceptions")
    endforeach()
    add_test(NAME dap_current_stdio COMMAND ${XRAY_PYTHON} ${XR_DAP_SCRIPT_DIR}/test_stdio.py
        --xray $<TARGET_FILE:xray> --fixture ${XR_DAP_FIXTURE_DIR}/wire_fixture.xr)
    set_tests_properties(dap_current_stdio PROPERTIES TIMEOUT 120 LABELS "dap;cli;stdio;execution;ownership")
    add_test(NAME dap_current_stdio_attach COMMAND ${XRAY_PYTHON} ${XR_DAP_SCRIPT_DIR}/test_stdio.py
        --xray $<TARGET_FILE:xray> --fixture ${XR_DAP_FIXTURE_DIR}/wire_fixture.xr --start-request attach)
    set_tests_properties(dap_current_stdio_attach PROPERTIES TIMEOUT 120 LABELS "dap;attach;cli;stdio;execution;ownership")
    add_test(NAME dap_current_tcp COMMAND ${XRAY_PYTHON} ${XR_DAP_SCRIPT_DIR}/test_stdio.py
        --xray $<TARGET_FILE:xray> --fixture ${XR_DAP_FIXTURE_DIR}/wire_fixture.xr --transport tcp)
    add_test(NAME dap_current_tcp_attach COMMAND ${XRAY_PYTHON} ${XR_DAP_SCRIPT_DIR}/test_stdio.py
        --xray $<TARGET_FILE:xray> --fixture ${XR_DAP_FIXTURE_DIR}/wire_fixture.xr --transport tcp --start-request attach)
    set_tests_properties(dap_current_tcp dap_current_tcp_attach PROPERTIES TIMEOUT 120 LABELS "dap;tcp;cli;execution;ownership")
    foreach(XR_DAP_DETACH_MODE running stopped unstarted timer attach output-closed)
        set(XR_DAP_DETACH_SOURCE threads_fixture.xr)
        if(XR_DAP_DETACH_MODE MATCHES "^(unstarted|output-closed)$")
            set(XR_DAP_DETACH_SOURCE wire_fixture.xr)
        elseif(XR_DAP_DETACH_MODE STREQUAL "timer")
            set(XR_DAP_DETACH_SOURCE timer_fixture.xr)
        endif()
        add_test(NAME dap_current_tcp_detach_${XR_DAP_DETACH_MODE} COMMAND ${XRAY_PYTHON} ${XR_DAP_SCRIPT_DIR}/test_stdio.py
            --xray $<TARGET_FILE:xray> --fixture ${XR_DAP_FIXTURE_DIR}/${XR_DAP_DETACH_SOURCE}
            --transport tcp --scenario detach-${XR_DAP_DETACH_MODE})
        set_tests_properties(dap_current_tcp_detach_${XR_DAP_DETACH_MODE} PROPERTIES TIMEOUT 120 LABELS "dap;tcp;detach;cli;execution;ownership")
    endforeach()

    configure_file(${PROJECT_SOURCE_DIR}/tests/regression/dap/fixtures/current/oracle.json ${XR_DAP_FIXTURE_DIR}/oracle.json COPYONLY)
    foreach(XR_DAP_WIRE_SCENARIO disconnect-clean stop-on-entry line-breakpoint logpoint mutate-oracle owned-oracle
        source-step-in source-next source-step-out condition-oracle hit-oracle replace-oracle)
        set(XR_DAP_WIRE_SOURCE call_stack.xr)
        if(XR_DAP_WIRE_SCENARIO MATCHES "^(logpoint|condition-oracle|hit-oracle|replace-oracle)$")
            set(XR_DAP_WIRE_SOURCE conditional_hits.xr)
        elseif(XR_DAP_WIRE_SCENARIO STREQUAL "owned-oracle")
            set(XR_DAP_WIRE_SOURCE owned_values.xr)
        endif()
        add_test(NAME dap_original_stdio_${XR_DAP_WIRE_SCENARIO} COMMAND ${XRAY_PYTHON} ${XR_DAP_SCRIPT_DIR}/test_stdio.py
            --xray $<TARGET_FILE:xray> --fixture ${XR_DAP_FIXTURE_DIR}/${XR_DAP_WIRE_SOURCE}
            --oracle ${XR_DAP_FIXTURE_DIR}/oracle.json --scenario ${XR_DAP_WIRE_SCENARIO})
        set_tests_properties(dap_original_stdio_${XR_DAP_WIRE_SCENARIO} PROPERTIES TIMEOUT 120 LABELS "dap;cli;stdio;source;execution;ownership;original-oracle")
    endforeach()
endif()

add_executable(test_dap_tcp test_tcp.c ${PROJECT_SOURCE_DIR}/src/base/xjson.c ${PROJECT_SOURCE_DIR}/src/base/xutf8.c)
target_link_libraries(test_dap_tcp PRIVATE xray_dap_current)
if(MSVC)
    target_compile_options(test_dap_tcp PRIVATE /W4 /WX)
else()
    target_compile_options(test_dap_tcp PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_dap_tcp COMMAND test_dap_tcp)
add_test(NAME test_dap_tcp_resources COMMAND test_dap_tcp --resources)
set_tests_properties(test_dap_tcp test_dap_tcp_resources PROPERTIES TIMEOUT 120 LABELS "dap;tcp;resources;ownership")

add_executable(test_dap_expression test_expression.c)
target_link_libraries(test_dap_expression PRIVATE xray_dap_current)
if(MSVC)
    target_compile_options(test_dap_expression PRIVATE /W4 /WX)
else()
    target_compile_options(test_dap_expression PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_dap_expression COMMAND test_dap_expression)
add_test(NAME test_dap_expression_resources COMMAND test_dap_expression --resources)
set_tests_properties(test_dap_expression test_dap_expression_resources PROPERTIES TIMEOUT 120 LABELS "dap;expression;parser;ownership;fault-injection;resources")


























































































add_test(NAME test_xir_source_text_facts COMMAND $<TARGET_FILE:test_xir_source_text> --facts)
set_tests_properties(test_xir_source_text_facts PROPERTIES TIMEOUT 120 LABELS "dap;source;metadata;ownership")




set(XR_DAP_NATIVE_ROUTES_DIR "${CMAKE_CURRENT_BINARY_DIR}/native-routes")






set(XR_DAP_NATIVE_PROVIDER_DIR "${CMAKE_CURRENT_BINARY_DIR}/native-provider")





# A separate build tree has no implicit source-relative standard library.
# Subprocess tests consume the same pinned sources as direct compiler fixtures.
get_property(XR_DAP_DIRECTORY_TESTS DIRECTORY PROPERTY TESTS)
foreach(XR_DAP_TEST IN LISTS XR_DAP_DIRECTORY_TESTS)
    if(XR_DAP_TEST MATCHES "^dap_(current|original)_")
        set_property(TEST ${XR_DAP_TEST} APPEND PROPERTY ENVIRONMENT_MODIFICATION
            "XRAY_STDLIB_PATH=set:${PROJECT_SOURCE_DIR}/stdlib")
    endif()
endforeach()
