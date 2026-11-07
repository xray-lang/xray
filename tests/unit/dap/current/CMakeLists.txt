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

add_executable(test_dap_native_throw_boundary test_native_throw_boundary.c ${XIR_CALL_GENERATED})
target_link_libraries(test_dap_native_throw_boundary PRIVATE xray_xir_scalar)
if(MSVC)
    target_compile_options(test_dap_native_throw_boundary PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_dap_native_throw_boundary PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_dap_native_throw_boundary COMMAND test_dap_native_throw_boundary)
set_tests_properties(test_dap_native_throw_boundary PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE LABELS "dap;native;ownership;execution")

add_executable(test_dap_native_task_producer test_native_task_producer.c)
target_link_libraries(test_dap_native_task_producer PRIVATE xray_xir_source xray_xir_cgen)
target_compile_definitions(test_dap_native_task_producer PRIVATE
    XR_NATIVE_TASK_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}" XR_SOURCE_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
set(XR_DAP_NATIVE_TASK_DIR "${CMAKE_CURRENT_BINARY_DIR}/native-task")
add_custom_command(OUTPUT "${XR_DAP_NATIVE_TASK_DIR}/native_task.c" "${XR_DAP_NATIVE_TASK_DIR}/native_task.h"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${XR_DAP_NATIVE_TASK_DIR}"
    COMMAND $<TARGET_FILE:test_dap_native_task_producer> "${XR_DAP_NATIVE_TASK_DIR}"
    DEPENDS test_dap_native_task_producer "${CMAKE_CURRENT_SOURCE_DIR}/native_task_throw.xr" VERBATIM)

foreach(XR_DAP_NATIVE_TASK_TARGET IN ITEMS test_dap_native_task_producer)
    if(MSVC)
        target_compile_options(${XR_DAP_NATIVE_TASK_TARGET} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${XR_DAP_NATIVE_TASK_TARGET} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()

add_executable(test_dap_native_source_producer test_native_task_producer.c)
target_link_libraries(test_dap_native_source_producer PRIVATE xray_xir_source xray_xir_cgen)
target_compile_definitions(test_dap_native_source_producer PRIVATE
    XR_NATIVE_TASK_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/native-source" XR_SOURCE_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
set(XR_DAP_NATIVE_SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/native-source")
add_custom_command(OUTPUT "${XR_DAP_NATIVE_SOURCE_DIR}/native_task.c" "${XR_DAP_NATIVE_SOURCE_DIR}/native_task.h"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${XR_DAP_NATIVE_SOURCE_DIR}"
    COMMAND $<TARGET_FILE:test_dap_native_source_producer> "${XR_DAP_NATIVE_SOURCE_DIR}"
    DEPENDS test_dap_native_source_producer "${CMAKE_CURRENT_SOURCE_DIR}/native-source/native_task_throw.xr" VERBATIM)

if(MSVC)

    target_compile_options(test_dap_native_source_producer PRIVATE /W4 /WX /utf-8)
else()

    target_compile_options(test_dap_native_source_producer PRIVATE -Wall -Wextra -Werror)
endif()

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

add_executable(test_dap_launch test_launch.c)
target_link_libraries(test_dap_launch PRIVATE xray_dap_current)
target_compile_definitions(test_dap_launch PRIVATE XR_DAP_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
if(MSVC)
    target_compile_options(test_dap_launch PRIVATE /W4 /WX)
else()
    target_compile_options(test_dap_launch PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_dap_launch COMMAND test_dap_launch)
add_test(NAME test_dap_launch_resources COMMAND test_dap_launch --resources)
set_tests_properties(test_dap_launch_resources PROPERTIES TIMEOUT 1800 COST 120 LABELS "dap;source;resources;fault-injection;ownership")
set_tests_properties(test_dap_launch PROPERTIES TIMEOUT 120 LABELS "dap;source;execution;ownership")

add_executable(test_dap_exception_policy test_exception_policy.c)
target_link_libraries(test_dap_exception_policy PRIVATE xray_dap_current)
target_compile_definitions(test_dap_exception_policy PRIVATE XR_DAP_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
if(MSVC)
    target_compile_options(test_dap_exception_policy PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_dap_exception_policy PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_dap_exception_policy COMMAND test_dap_exception_policy)
add_test(NAME test_dap_exception_policy_resources COMMAND test_dap_exception_policy --resources)
set_tests_properties(test_dap_exception_policy_resources PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1 LABELS "dap;source;errors;ownership;metadata;filters;resources;fault-injection")
set_tests_properties(test_dap_exception_policy PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1 LABELS "dap;source;errors;ownership;metadata;filters")

add_executable(test_dap_source_routes test_source_routes.c)
target_link_libraries(test_dap_source_routes PRIVATE xray_dap_current)
target_compile_definitions(test_dap_source_routes PRIVATE XR_DAP_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
if(MSVC)
    target_compile_options(test_dap_source_routes PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_dap_source_routes PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_dap_source_routes COMMAND test_dap_source_routes)
add_test(NAME test_dap_source_routes_resources COMMAND test_dap_source_routes --resources)
set_tests_properties(test_dap_source_routes test_dap_source_routes_resources PROPERTIES
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1 LABELS "dap;source;errors;ownership;metadata")
set_property(TEST test_dap_source_routes_resources APPEND PROPERTY LABELS "resources;fault-injection")

add_executable(test_dap_dispatch test_dispatch.c ${PROJECT_SOURCE_DIR}/src/base/xjson.c ${PROJECT_SOURCE_DIR}/src/base/xutf8.c)
target_link_libraries(test_dap_dispatch PRIVATE xray_dap_current)
target_compile_definitions(test_dap_dispatch PRIVATE XR_DAP_STDLIB="${PROJECT_SOURCE_DIR}/stdlib" XR_DAP_FIXTURE="${XR_DAP_FIXTURE_DIR}/wire_fixture.xr")
if(MSVC)
    target_compile_options(test_dap_dispatch PRIVATE /W4 /WX)
else()
    target_compile_options(test_dap_dispatch PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_dap_dispatch COMMAND test_dap_dispatch)
set_tests_properties(test_dap_dispatch PROPERTIES TIMEOUT 120 LABELS "dap;json;source;execution;ownership")

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

add_executable(test_dap_expression_product test_expression_product.c)
target_link_libraries(test_dap_expression_product PRIVATE xray_dap_current)
target_compile_definitions(test_dap_expression_product PRIVATE XR_DAP_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
if(MSVC)
    target_compile_options(test_dap_expression_product PRIVATE /W4 /WX)
else()
    target_compile_options(test_dap_expression_product PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_dap_expression_product COMMAND test_dap_expression_product)
add_test(NAME test_dap_expression_product_resources COMMAND test_dap_expression_product --resources)
set_tests_properties(test_dap_expression_product test_dap_expression_product_resources PROPERTIES TIMEOUT 120 LABELS "dap;expression;ctfe;source;ownership;fault-injection;resources")

add_executable(test_dap_evaluate test_evaluate.c ${PROJECT_SOURCE_DIR}/src/base/xjson.c ${PROJECT_SOURCE_DIR}/src/base/xutf8.c)
target_link_libraries(test_dap_evaluate PRIVATE xray_dap_current)
target_compile_definitions(test_dap_evaluate PRIVATE XR_DAP_STDLIB="${PROJECT_SOURCE_DIR}/stdlib"
    XR_DAP_FIXTURE="${XR_DAP_FIXTURE_DIR}/wire_fixture.xr" XR_DAP_CALL_FIXTURE="${XR_DAP_FIXTURE_DIR}/call_stack.xr"
    XR_DAP_MEMBER_FIXTURE="${XR_DAP_FIXTURE_DIR}/member_expressions.xr"
    XR_DAP_EXPRESSION_FIXTURE="${XR_DAP_FIXTURE_DIR}/expression_data.xr")
if(MSVC)
    target_compile_options(test_dap_evaluate PRIVATE /W4 /WX)
else()
    target_compile_options(test_dap_evaluate PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_dap_evaluate COMMAND test_dap_evaluate)
add_test(NAME test_dap_evaluate_resources COMMAND test_dap_evaluate --resources)
add_test(NAME test_dap_evaluate_data COMMAND test_dap_evaluate --managed)
add_test(NAME test_dap_evaluate_members COMMAND test_dap_evaluate --members)
add_test(NAME test_dap_evaluate_members_resources COMMAND test_dap_evaluate --members-resources)
set_tests_properties(test_dap_evaluate_members_resources PROPERTIES TIMEOUT 1800 COST 180 LABELS "dap;evaluate;source;ownership;resources;fault-injection;nominal" RUN_SERIAL TRUE)
set_tests_properties(test_dap_evaluate_members PROPERTIES TIMEOUT 120 LABELS "dap;evaluate;source;ownership;nominal")
add_test(NAME test_dap_evaluate_data_resources COMMAND test_dap_evaluate --managed-resources)
set_tests_properties(test_dap_evaluate_data_resources PROPERTIES TIMEOUT 120 LABELS "dap;evaluate;source;ownership;data;resources;fault-injection")
set_tests_properties(test_dap_evaluate_data PROPERTIES TIMEOUT 120 LABELS "dap;evaluate;source;ownership;data")
set_tests_properties(test_dap_evaluate test_dap_evaluate_resources PROPERTIES TIMEOUT 120 LABELS "dap;evaluate;ctfe;source;ownership;fault-injection;resources")

add_executable(test_dap_write test_write.c ${PROJECT_SOURCE_DIR}/src/base/xjson.c ${PROJECT_SOURCE_DIR}/src/base/xutf8.c)
target_link_libraries(test_dap_write PRIVATE xray_dap_current)
target_compile_definitions(test_dap_write PRIVATE XR_DAP_STDLIB="${PROJECT_SOURCE_DIR}/stdlib"
    XR_DAP_FIXTURE="${XR_DAP_FIXTURE_DIR}/wire_fixture.xr" XR_DAP_CALL_FIXTURE="${XR_DAP_FIXTURE_DIR}/call_stack.xr"
    XR_DAP_THREADS_FIXTURE="${XR_DAP_FIXTURE_DIR}/threads_fixture.xr")
if(MSVC)
    target_compile_options(test_dap_write PRIVATE /W4 /WX)
else()
    target_compile_options(test_dap_write PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_dap_write COMMAND test_dap_write)
add_test(NAME test_dap_write_resources COMMAND test_dap_write --resources)
set_tests_properties(test_dap_write test_dap_write_resources PROPERTIES TIMEOUT 120 LABELS "dap;mutation;source;ownership;resources;fault-injection")

add_executable(test_dap_projection_write test_projection_write.c ${PROJECT_SOURCE_DIR}/src/base/xjson.c ${PROJECT_SOURCE_DIR}/src/base/xutf8.c)
target_link_libraries(test_dap_projection_write PRIVATE xray_dap_current)
target_compile_definitions(test_dap_projection_write PRIVATE XR_DAP_STDLIB="${PROJECT_SOURCE_DIR}/stdlib"
    XR_DAP_FIXTURE="${XR_DAP_FIXTURE_DIR}/wire_fixture.xr" XR_DAP_CALL_FIXTURE="${XR_DAP_FIXTURE_DIR}/call_stack.xr"
    XR_DAP_THREADS_FIXTURE="${XR_DAP_FIXTURE_DIR}/threads_fixture.xr" XR_DAP_PROJECTION_FIXTURE="${XR_DAP_FIXTURE_DIR}/write_projection.xr")
if(MSVC)
    target_compile_options(test_dap_projection_write PRIVATE /W4 /WX)
else()
    target_compile_options(test_dap_projection_write PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_dap_projection_write COMMAND test_dap_projection_write)
add_test(NAME test_dap_projection_write_resources COMMAND test_dap_projection_write --resources)
set_tests_properties(test_dap_projection_write test_dap_projection_write_resources PROPERTIES TIMEOUT 120 LABELS "dap;mutation;source;ownership;resources;fault-injection")

add_executable(test_dap_origin test_origin.c ${PROJECT_SOURCE_DIR}/src/base/xjson.c ${PROJECT_SOURCE_DIR}/src/base/xutf8.c)
target_link_libraries(test_dap_origin PRIVATE xray_dap_current)
target_compile_definitions(test_dap_origin PRIVATE XR_DAP_STDLIB="${PROJECT_SOURCE_DIR}/stdlib"
    XR_DAP_FIXTURE="${XR_DAP_FIXTURE_DIR}/wire_fixture.xr" XR_DAP_CALL_FIXTURE="${XR_DAP_FIXTURE_DIR}/call_stack.xr"
    XR_DAP_THREADS_FIXTURE="${XR_DAP_FIXTURE_DIR}/threads_fixture.xr" XR_DAP_ORIGIN_FIXTURE="${XR_DAP_FIXTURE_DIR}/write_projection.xr")
if(MSVC)
    target_compile_options(test_dap_origin PRIVATE /W4 /WX)
else()
    target_compile_options(test_dap_origin PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_dap_origin COMMAND test_dap_origin)
add_test(NAME test_dap_origin_resources COMMAND test_dap_origin --resources)
set_tests_properties(test_dap_origin test_dap_origin_resources PROPERTIES TIMEOUT 120 LABELS "dap;source;ownership;resources;fault-injection")

add_executable(test_dap_child_write test_child_write.c ${PROJECT_SOURCE_DIR}/src/base/xjson.c ${PROJECT_SOURCE_DIR}/src/base/xutf8.c)
target_link_libraries(test_dap_child_write PRIVATE xray_dap_current)
target_compile_definitions(test_dap_child_write PRIVATE XR_DAP_STDLIB="${PROJECT_SOURCE_DIR}/stdlib"
    XR_DAP_FIXTURE="${XR_DAP_FIXTURE_DIR}/wire_fixture.xr" XR_DAP_CALL_FIXTURE="${XR_DAP_FIXTURE_DIR}/call_stack.xr"
    XR_DAP_THREADS_FIXTURE="${XR_DAP_FIXTURE_DIR}/threads_fixture.xr" XR_DAP_ORIGIN_FIXTURE="${XR_DAP_FIXTURE_DIR}/write_projection.xr"
    XR_DAP_CHILD_WRITE_FIXTURE="${XR_DAP_FIXTURE_DIR}/child_write.xr")
if(MSVC)
    target_compile_options(test_dap_child_write PRIVATE /W4 /WX)
else()
    target_compile_options(test_dap_child_write PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_dap_child_write COMMAND test_dap_child_write)
add_test(NAME test_dap_child_write_resources COMMAND test_dap_child_write --resources)
set_tests_properties(test_dap_child_write test_dap_child_write_resources PROPERTIES TIMEOUT 120 LABELS "dap;mutation;source;ownership;resources;fault-injection")

add_executable(test_dap_ref_write test_ref_write.c ${PROJECT_SOURCE_DIR}/src/base/xjson.c ${PROJECT_SOURCE_DIR}/src/base/xutf8.c)
target_link_libraries(test_dap_ref_write PRIVATE xray_dap_current)
target_compile_definitions(test_dap_ref_write PRIVATE XR_DAP_STDLIB="${PROJECT_SOURCE_DIR}/stdlib"
    XR_DAP_FIXTURE="${XR_DAP_FIXTURE_DIR}/wire_fixture.xr" XR_DAP_CALL_FIXTURE="${XR_DAP_FIXTURE_DIR}/call_stack.xr"
    XR_DAP_THREADS_FIXTURE="${XR_DAP_FIXTURE_DIR}/threads_fixture.xr" XR_DAP_ORIGIN_FIXTURE="${XR_DAP_FIXTURE_DIR}/write_projection.xr"
    XR_DAP_CHILD_WRITE_FIXTURE="${XR_DAP_FIXTURE_DIR}/child_write.xr" XR_DAP_REF_CLASS_FIXTURE="${XR_DAP_FIXTURE_DIR}/ref_class.xr"
    XR_DAP_REF_VALUES_FIXTURE="${XR_DAP_FIXTURE_DIR}/ref_values.xr" XR_DAP_REF_WHOLE_FIXTURE="${XR_DAP_FIXTURE_DIR}/ref_whole.xr"
    XR_DAP_REF_CHILD_SCALAR_FIXTURE="${XR_DAP_FIXTURE_DIR}/ref_child_scalar.xr" XR_DAP_REF_CHILD_ARRAY_FIXTURE="${XR_DAP_FIXTURE_DIR}/ref_child_array.xr")
if(MSVC)
    target_compile_options(test_dap_ref_write PRIVATE /W4 /WX)
else()
    target_compile_options(test_dap_ref_write PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_dap_ref_write COMMAND test_dap_ref_write)
add_test(NAME test_dap_ref_write_resources COMMAND test_dap_ref_write --resources)
set_tests_properties(test_dap_ref_write test_dap_ref_write_resources PROPERTIES TIMEOUT 120 LABELS "dap;mutation;source;ownership;resources;fault-injection")

add_executable(test_dap_class_write test_class_write.c ${PROJECT_SOURCE_DIR}/src/base/xjson.c ${PROJECT_SOURCE_DIR}/src/base/xutf8.c)
target_link_libraries(test_dap_class_write PRIVATE xray_dap_current)
target_compile_definitions(test_dap_class_write PRIVATE XR_DAP_STDLIB="${PROJECT_SOURCE_DIR}/stdlib"
    XR_DAP_FIXTURE="${XR_DAP_FIXTURE_DIR}/wire_fixture.xr" XR_DAP_CALL_FIXTURE="${XR_DAP_FIXTURE_DIR}/call_stack.xr"
    XR_DAP_THREADS_FIXTURE="${XR_DAP_FIXTURE_DIR}/threads_fixture.xr" XR_DAP_ORIGIN_FIXTURE="${XR_DAP_FIXTURE_DIR}/write_projection.xr"
    XR_DAP_CHILD_WRITE_FIXTURE="${XR_DAP_FIXTURE_DIR}/child_write.xr" XR_DAP_CLASS_WRITE_FIXTURE="${XR_DAP_FIXTURE_DIR}/class_write.xr"
    XR_DAP_CLASS_CHILD_FIXTURE="${XR_DAP_FIXTURE_DIR}/class_child_write.xr" XR_DAP_CLASS_MODULE_FIXTURE="${XR_DAP_FIXTURE_DIR}/class_module_write.xr"
    XR_DAP_CLASS_LOAN_FIXTURE="${XR_DAP_FIXTURE_DIR}/ref_class.xr" XR_DAP_CLASS_ALIAS_FIXTURE="${XR_DAP_FIXTURE_DIR}/class_alias_write.xr"
    XR_DAP_CLASS_WRAPPER_FIXTURE="${XR_DAP_FIXTURE_DIR}/class_wrapper_write.xr"
    XR_DAP_CLASS_WRAPPER_CHILD_FIXTURE="${XR_DAP_FIXTURE_DIR}/class_wrapper_child_write.xr")
if(MSVC)
    target_compile_options(test_dap_class_write PRIVATE /W4 /WX)
else()
    target_compile_options(test_dap_class_write PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_dap_class_write COMMAND test_dap_class_write)
add_test(NAME test_dap_class_write_resources COMMAND test_dap_class_write --resources)
set_tests_properties(test_dap_class_write test_dap_class_write_resources PROPERTIES TIMEOUT 120 LABELS "dap;mutation;source;ownership;resources;fault-injection")

add_executable(test_dap_wrapper_rebind test_wrapper_rebind.c ${PROJECT_SOURCE_DIR}/src/base/xjson.c ${PROJECT_SOURCE_DIR}/src/base/xutf8.c)
target_link_libraries(test_dap_wrapper_rebind PRIVATE xray_dap_current)
target_compile_definitions(test_dap_wrapper_rebind PRIVATE XR_DAP_STDLIB="${PROJECT_SOURCE_DIR}/stdlib"
    XR_DAP_FIXTURE="${XR_DAP_FIXTURE_DIR}/wire_fixture.xr" XR_DAP_CALL_FIXTURE="${XR_DAP_FIXTURE_DIR}/call_stack.xr"
    XR_DAP_THREADS_FIXTURE="${XR_DAP_FIXTURE_DIR}/threads_fixture.xr" XR_DAP_ORIGIN_FIXTURE="${XR_DAP_FIXTURE_DIR}/write_projection.xr"
    XR_DAP_CHILD_WRITE_FIXTURE="${XR_DAP_FIXTURE_DIR}/child_write.xr"
    XR_DAP_REBIND_FIXTURE="${XR_DAP_FIXTURE_DIR}/wrapper_rebind.xr" XR_DAP_REBIND_CHILD_FIXTURE="${XR_DAP_FIXTURE_DIR}/wrapper_rebind_child.xr")
if(MSVC)
    target_compile_options(test_dap_wrapper_rebind PRIVATE /W4 /WX)
else()
    target_compile_options(test_dap_wrapper_rebind PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_dap_wrapper_rebind_root COMMAND test_dap_wrapper_rebind --root)
add_test(NAME test_dap_wrapper_rebind_child COMMAND test_dap_wrapper_rebind --child)
add_test(NAME test_dap_wrapper_rebind_root_resources COMMAND test_dap_wrapper_rebind --root --resources)
add_test(NAME test_dap_wrapper_rebind_child_resources COMMAND test_dap_wrapper_rebind --child --resources)
set_tests_properties(test_dap_wrapper_rebind_root test_dap_wrapper_rebind_child test_dap_wrapper_rebind_root_resources test_dap_wrapper_rebind_child_resources
    PROPERTIES TIMEOUT 120 LABELS "dap;mutation;source;ownership;resources;fault-injection")

add_test(NAME test_dap_lexical_visibility COMMAND test_dap_evaluate --visibility)
add_test(NAME test_dap_lexical_visibility_resources COMMAND test_dap_evaluate --visibility-resources)
set_tests_properties(test_dap_lexical_visibility test_dap_lexical_visibility_resources PROPERTIES TIMEOUT 120 LABELS "dap;source;ownership;scope;fault-injection")

add_test(NAME test_dap_lexical_visibility_source_resources COMMAND test_dap_evaluate --visibility-source-resources)
set_tests_properties(test_dap_lexical_visibility_source_resources PROPERTIES TIMEOUT 1800 COST 120 LABELS "dap;source;ownership;scope;fault-injection")

add_test(NAME test_xir_source_text_facts COMMAND $<TARGET_FILE:test_xir_source_text> --facts)
set_tests_properties(test_xir_source_text_facts PROPERTIES TIMEOUT 120 LABELS "dap;source;metadata;ownership")

add_executable(test_dap_native_routes_producer test_native_routes_producer.c)
target_link_libraries(test_dap_native_routes_producer PRIVATE xray_xir_source xray_xir_cgen)
target_compile_definitions(test_dap_native_routes_producer PRIVATE
    XR_NATIVE_TASK_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/native-routes" XR_SOURCE_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
set(XR_DAP_NATIVE_ROUTES_DIR "${CMAKE_CURRENT_BINARY_DIR}/native-routes")
add_custom_command(OUTPUT "${XR_DAP_NATIVE_ROUTES_DIR}/native_task.c" "${XR_DAP_NATIVE_ROUTES_DIR}/native_task.h"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${XR_DAP_NATIVE_ROUTES_DIR}"
    COMMAND $<TARGET_FILE:test_dap_native_routes_producer> "${XR_DAP_NATIVE_ROUTES_DIR}"
    DEPENDS test_dap_native_routes_producer "${CMAKE_CURRENT_SOURCE_DIR}/native-routes/native_task_throw.xr" VERBATIM)

if(MSVC)

    target_compile_options(test_dap_native_routes_producer PRIVATE /W4 /WX /utf-8)
else()

    target_compile_options(test_dap_native_routes_producer PRIVATE -Wall -Wextra -Werror)
endif()

add_executable(test_dap_native_provider_producer test_native_provider_producer.c)
target_link_libraries(test_dap_native_provider_producer PRIVATE xray_source_compile xray_xir_cgen)
set(XR_DAP_NATIVE_PROVIDER_DIR "${CMAKE_CURRENT_BINARY_DIR}/native-provider")
target_compile_definitions(test_dap_native_provider_producer PRIVATE
    XR_NATIVE_PROVIDER_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/native-provider/program.xr" XR_SOURCE_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
add_custom_command(OUTPUT "${XR_DAP_NATIVE_PROVIDER_DIR}/native_provider.c" "${XR_DAP_NATIVE_PROVIDER_DIR}/native_provider.h"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${XR_DAP_NATIVE_PROVIDER_DIR}"
    COMMAND $<TARGET_FILE:test_dap_native_provider_producer> "${XR_DAP_NATIVE_PROVIDER_DIR}"
    DEPENDS test_dap_native_provider_producer "${CMAKE_CURRENT_SOURCE_DIR}/native-provider/program.xr" VERBATIM)

if(MSVC)
    target_compile_options(test_dap_native_provider_producer PRIVATE /W4 /WX /utf-8)

else()
    target_compile_options(test_dap_native_provider_producer PRIVATE -Wall -Wextra -Werror)

endif()

# A separate build tree has no implicit source-relative standard library.
# Subprocess tests consume the same pinned sources as direct compiler fixtures.
get_property(XR_DAP_DIRECTORY_TESTS DIRECTORY PROPERTY TESTS)
foreach(XR_DAP_TEST IN LISTS XR_DAP_DIRECTORY_TESTS)
    if(XR_DAP_TEST MATCHES "^dap_(current|original)_")
        set_property(TEST ${XR_DAP_TEST} APPEND PROPERTY ENVIRONMENT_MODIFICATION
            "XRAY_STDLIB_PATH=set:${PROJECT_SOURCE_DIR}/stdlib")
    endif()
endforeach()
