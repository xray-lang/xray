add_executable(test_hir_body test_body.c)
target_link_libraries(test_hir_body PRIVATE xray_xir_source)
target_include_directories(test_hir_body PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_executable(test_hir_body_source test_body_source.c)
add_executable(test_hir_tuple_control test_tuple_source_control.c)
add_executable(test_hir_nominal_control test_nominal_source_control.c)
target_link_libraries(test_hir_nominal_control PRIVATE xray_xir_source)
target_include_directories(test_hir_nominal_control PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(test_hir_tuple_control PRIVATE xray_xir_source)
target_include_directories(test_hir_tuple_control PRIVATE "${PROJECT_SOURCE_DIR}/src")
if(WIN32)
    target_link_libraries(test_hir_body_source PRIVATE ${XRAY_SOURCE_SDK_ARCHIVE_TARGETS})
else()
    target_link_libraries(test_hir_body_source PRIVATE xray_source_api)
endif()
target_include_directories(test_hir_body_source PRIVATE "${PROJECT_SOURCE_DIR}/include")
if(MSVC)
    set_target_properties(test_hir_body_source PROPERTIES MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
endif()
foreach(target test_hir_body test_hir_body_source test_hir_tuple_control test_hir_nominal_control)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -pedantic)
    endif()
endforeach()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/empty-stdlib")
add_test(NAME hir_body COMMAND test_hir_body)
add_test(NAME hir_tuple_source_control COMMAND test_hir_tuple_control "${CMAKE_CURRENT_SOURCE_DIR}/tuple_contracts.xr")
set_tests_properties(hir_tuple_source_control PROPERTIES TIMEOUT 120 LABELS "unit;hir;types;source;ctfe;ownership;resources")
add_test(NAME hir_nominal_source_control COMMAND test_hir_nominal_control "${CMAKE_CURRENT_SOURCE_DIR}/nominal_contracts.xr")
set_tests_properties(hir_nominal_source_control PROPERTIES TIMEOUT 120 LABELS "unit;hir;types;source;ctfe;ownership;resources")
set_tests_properties(hir_body PROPERTIES FIXTURES_SETUP hir_body_inputs WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME source_body_sdk COMMAND test_hir_body_source "${CMAKE_CURRENT_BINARY_DIR}/empty-stdlib"
    "${CMAKE_CURRENT_BINARY_DIR}/control.xr" "${CMAKE_CURRENT_BINARY_DIR}/control.out"
    "${PROJECT_SOURCE_DIR}/tests/diff/cases/semantics/string_owning_parameter.xr" "${CMAKE_CURRENT_BINARY_DIR}/original.out"
    "${CMAKE_CURRENT_BINARY_DIR}/uses.xr" "${CMAKE_CURRENT_BINARY_DIR}/uses.out"
    "${CMAKE_CURRENT_BINARY_DIR}/places.xr" "${CMAKE_CURRENT_BINARY_DIR}/places.out"
    "${CMAKE_CURRENT_BINARY_DIR}/callback.xr" "${CMAKE_CURRENT_BINARY_DIR}/callback.out"
    "${CMAKE_CURRENT_BINARY_DIR}/callback-family.xr" "${CMAKE_CURRENT_BINARY_DIR}/callback-family.out"
    "${CMAKE_CURRENT_BINARY_DIR}/handlers.xr" "${CMAKE_CURRENT_BINARY_DIR}/handlers.out"
    "${CMAKE_CURRENT_BINARY_DIR}/core-calls.xr" "${CMAKE_CURRENT_BINARY_DIR}/core-calls.out"
    "${CMAKE_CURRENT_BINARY_DIR}/inferred-callables.xr" "${CMAKE_CURRENT_BINARY_DIR}/inferred-callables.out"
    "${CMAKE_CURRENT_SOURCE_DIR}/tuple_contracts.xr" "${CMAKE_CURRENT_SOURCE_DIR}/tuple_contracts.out"
    "${CMAKE_CURRENT_SOURCE_DIR}/nominal_contracts.xr" "${CMAKE_CURRENT_SOURCE_DIR}/nominal_contracts.out"
    "${CMAKE_CURRENT_SOURCE_DIR}/nominal_order.xr" "${CMAKE_CURRENT_SOURCE_DIR}/nominal_order.out")
set_tests_properties(source_body_sdk PROPERTIES FIXTURES_REQUIRED hir_body_inputs)
add_test(NAME source_body_dead_handler_type COMMAND test_hir_body_source --expect-rejection "${CMAKE_CURRENT_BINARY_DIR}/empty-stdlib"
    "${CMAKE_CURRENT_BINARY_DIR}/handler-bad-type.xr" "${CMAKE_CURRENT_BINARY_DIR}/handler-bad-type.out")
add_test(NAME source_body_dead_handler_name COMMAND test_hir_body_source --expect-rejection "${CMAKE_CURRENT_BINARY_DIR}/empty-stdlib"
    "${CMAKE_CURRENT_BINARY_DIR}/handler-bad-name.xr" "${CMAKE_CURRENT_BINARY_DIR}/handler-bad-name.out")
set_tests_properties(source_body_dead_handler_type source_body_dead_handler_name PROPERTIES FIXTURES_REQUIRED hir_body_inputs)
add_test(NAME source_body_generic_comptime COMMAND test_hir_body_source "${CMAKE_CURRENT_BINARY_DIR}/empty-stdlib"
    "${CMAKE_CURRENT_BINARY_DIR}/generic-comptime.xr" "${CMAKE_CURRENT_BINARY_DIR}/generic-comptime.out")
set_tests_properties(source_body_generic_comptime PROPERTIES FIXTURES_REQUIRED hir_body_inputs)
set_tests_properties(hir_body source_body_sdk source_body_dead_handler_type source_body_dead_handler_name source_body_generic_comptime
    PROPERTIES TIMEOUT 120 LABELS "unit;hir;types;ownership;sdk")
if(WIN32)
    add_test(NAME hir_body_cli COMMAND ${XRAY_PYTHON} -X utf8 "${CMAKE_CURRENT_SOURCE_DIR}/test_body_cli.py"
        --root "${PROJECT_SOURCE_DIR}" --binary $<TARGET_FILE:xray>
        --client $<TARGET_FILE:test_hir_body_source> --pipeline $<TARGET_FILE:lir_pipeline_eval>)
    set_tests_properties(hir_body_cli PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE LABELS "unit;hir;types;cli;native;ownership")
endif()
