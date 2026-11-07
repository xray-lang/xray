
add_executable(test_source_public_repl "${CMAKE_CURRENT_LIST_DIR}/source_public/test_source_public_repl.c")
target_link_libraries(test_source_public_repl PRIVATE xray_source_api)
target_compile_definitions(test_source_public_repl PRIVATE XR_PUBLIC_STDLIB="${PROJECT_SOURCE_DIR}/stdlib"
    XR_PUBLIC_REPL_FIXTURE="${CMAKE_CURRENT_LIST_DIR}/source_public_repl_rooted"
    XR_PUBLIC_REPL_FILE_FIXTURE="${CMAKE_CURRENT_LIST_DIR}/source_public_repl_file"
    XR_PUBLIC_REPL_FILE_TEMP_ROOT="${CMAKE_CURRENT_BINARY_DIR}")
set_target_properties(test_source_public_repl PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_public_repl PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_source_public_repl PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME source_public_repl COMMAND test_source_public_repl)
set_tests_properties(source_public_repl PROPERTIES TIMEOUT 120 LABELS "unit;api;source;ownership;repl")
add_test(NAME source_public_repl_file COMMAND test_source_public_repl --files)
set_tests_properties(source_public_repl_file PROPERTIES TIMEOUT 120 LABELS "unit;api;source;ownership;repl;authority;file")
add_test(NAME source_public_repl_rooted COMMAND test_source_public_repl --rooted)
set_tests_properties(source_public_repl_rooted PROPERTIES TIMEOUT 120 LABELS "unit;api;source;ownership;repl;authority")

add_test(NAME source_public_repl_partition_protocol COMMAND ${Python3_EXECUTABLE}
    "${CMAKE_CURRENT_LIST_DIR}/source_public/test_repl_fault_partitions.py")
set_tests_properties(source_public_repl_partition_protocol PROPERTIES TIMEOUT 30
    LABELS "unit;api;source;ownership;repl;compiler-faults;protocol")

add_executable(test_source_public_instance "${CMAKE_CURRENT_LIST_DIR}/source_public/test_source_public_instance.c")
target_link_libraries(test_source_public_instance PRIVATE xray_source_api)
target_compile_definitions(test_source_public_instance PRIVATE XR_PUBLIC_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
set_target_properties(test_source_public_instance PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_public_instance PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_source_public_instance PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME source_public_instance COMMAND test_source_public_instance)
set_tests_properties(source_public_instance PROPERTIES TIMEOUT 120 LABELS "unit;api;source;ownership;call;values")

add_test(NAME source_public_repl_input COMMAND test_source_public_repl --input)
set_tests_properties(source_public_repl_input PROPERTIES TIMEOUT 120 LABELS "unit;api;source;repl")

add_test(NAME source_public_repl_array_history COMMAND test_source_public_repl --array-history)

set_tests_properties(source_public_repl_array_history PROPERTIES TIMEOUT 120 LABELS "unit;api;source;repl;array;ownership;budget")

add_test(NAME source_public_repl_write COMMAND test_source_public_repl --write)

set_tests_properties(source_public_repl_write PROPERTIES TIMEOUT 120 LABELS "unit;api;source;repl;write;ownership;budget")

add_test(NAME source_public_repl_write_class_history COMMAND test_source_public_repl --write-class-history)
set_tests_properties(source_public_repl_write_class_history PROPERTIES TIMEOUT 120 LABELS "unit;api;source;repl;write;class;ownership")

add_test(NAME source_input_cli_diagnostics COMMAND ${Python3_EXECUTABLE}
    "${CMAKE_CURRENT_LIST_DIR}/source_public/run_source_input_cli.py"
    --root "${PROJECT_SOURCE_DIR}" --cli $<TARGET_FILE:xray>
    --record "${CMAKE_CURRENT_BINARY_DIR}/source-input-cli-record.json")
set_tests_properties(source_input_cli_diagnostics PROPERTIES TIMEOUT 180 LABELS "cli;source;ownership;diagnostic")

add_executable(test_source_request_lifetime "${CMAKE_CURRENT_LIST_DIR}/source_public/test_source_request_lifetime.c"
    "${CMAKE_CURRENT_LIST_DIR}/source_public/nominal_projection_probe.c"
    "${CMAKE_CURRENT_LIST_DIR}/source_public/declarations_copy_probe.c")
target_link_libraries(test_source_request_lifetime PRIVATE xray_source_api)
target_compile_definitions(test_source_request_lifetime PRIVATE XR_PUBLIC_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
set_target_properties(test_source_request_lifetime PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_request_lifetime PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_source_request_lifetime PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME source_request_lifetime COMMAND test_source_request_lifetime)
set_tests_properties(source_request_lifetime PROPERTIES TIMEOUT 120 LABELS "unit;api;source;ownership")
add_test(NAME source_implementation_control COMMAND test_source_request_lifetime --implementation-control)
set_tests_properties(source_implementation_control PROPERTIES TIMEOUT 120 PROCESSORS 1 LABELS "unit;api;source;ownership;resources")
add_test(NAME source_packet_request_control COMMAND test_source_request_lifetime --packet-codec)
add_test(NAME source_packet_writer_control COMMAND test_source_request_lifetime --source-writer)
add_test(NAME source_product_facts_control COMMAND test_source_request_lifetime --product-facts)
set_tests_properties(source_product_facts_control PROPERTIES TIMEOUT 120 PROCESSORS 1 LABELS "unit;api;source;ownership;resources")
set_tests_properties(source_packet_writer_control PROPERTIES TIMEOUT 120 PROCESSORS 1 LABELS "unit;api;source;ownership;resources")
set_tests_properties(source_packet_request_control PROPERTIES
    TIMEOUT 120 LABELS "unit;api;source;ownership;cancel;deadline")

add_test(NAME source_product_packet_lifetime COMMAND test_source_request_lifetime --product-packets)
set_tests_properties(source_product_packet_lifetime PROPERTIES TIMEOUT 120 PROCESSORS 1 LABELS "unit;api;source;ownership;resources")
add_test(NAME source_ctfe_cleanup_lifetime COMMAND test_source_request_lifetime --ctfe-cleanup)
set_tests_properties(source_ctfe_cleanup_lifetime PROPERTIES TIMEOUT 120
    LABELS "unit;api;source;ctfe;cleanup;ownership")
foreach(case baseline cancel_functions cancel_module_name cancel_publication deadline_functions deadline_publication oom_functions)
    add_test(NAME source_declarations_control_${case} COMMAND test_source_request_lifetime --declarations-control ${case})
    set_tests_properties(source_declarations_control_${case} PROPERTIES TIMEOUT 240 PROCESSORS 1 LABELS "unit;api;source;ownership;resources")
endforeach()
set_tests_properties(source_declarations_control_baseline PROPERTIES COST 47)
set_tests_properties(source_declarations_control_cancel_functions PROPERTIES COST 47)
set_tests_properties(source_declarations_control_cancel_module_name PROPERTIES COST 48)
set_tests_properties(source_declarations_control_cancel_publication PROPERTIES COST 51)
set_tests_properties(source_declarations_control_deadline_functions PROPERTIES COST 164)
set_tests_properties(source_declarations_control_deadline_publication PROPERTIES COST 149)
set_tests_properties(source_declarations_control_oom_functions PROPERTIES COST 66)

add_executable(test_source_request_ast_boundary "${CMAKE_CURRENT_LIST_DIR}/source_public/test_source_request_ast_boundary.c")
target_link_libraries(test_source_request_ast_boundary PRIVATE xray_source_api)
target_include_directories(test_source_request_ast_boundary PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_definitions(test_source_request_ast_boundary PRIVATE XR_PUBLIC_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
set_target_properties(test_source_request_ast_boundary PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_request_ast_boundary PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_source_request_ast_boundary PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME source_request_ast_boundary COMMAND test_source_request_ast_boundary)
set_tests_properties(source_request_ast_boundary PROPERTIES TIMEOUT 120 LABELS "unit;api;source;ownership")

# The original C11 embedding scenarios consume the real request/artifact API.
add_executable(test_source_sdk_consumer "${PROJECT_SOURCE_DIR}/tests/unit/xir/source_sdk_consumer.c")
target_link_libraries(test_source_sdk_consumer PRIVATE xray_source_api)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/source-sdk-empty-stdlib")
add_test(NAME source_sdk_c11_consumer COMMAND test_source_sdk_consumer "${CMAKE_CURRENT_BINARY_DIR}/source-sdk-empty-stdlib")
set_tests_properties(source_sdk_c11_consumer PROPERTIES TIMEOUT 120 LABELS "unit;api;source;ownership;sdk")

add_executable(test_ctfe_resource_modes "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_ctfe_resource_modes.c")
target_link_libraries(test_ctfe_resource_modes PRIVATE xray_xir_ctfe)
add_test(NAME ctfe_resource_modes COMMAND test_ctfe_resource_modes)
set_tests_properties(ctfe_resource_modes PROPERTIES TIMEOUT 120 LABELS "unit;compiler;ctfe;ownership")
foreach(target test_source_sdk_consumer test_ctfe_resource_modes)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
        if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
            target_compile_options(${target} PRIVATE /experimental:c11atomics)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -pedantic)
    endif()
endforeach()

add_executable(test_source_request_empty_enum "${CMAKE_CURRENT_LIST_DIR}/source_public/test_source_request_empty_enum.c")
target_link_libraries(test_source_request_empty_enum PRIVATE xray_source_api)
target_compile_definitions(test_source_request_empty_enum PRIVATE XR_PUBLIC_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
set_target_properties(test_source_request_empty_enum PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_request_empty_enum PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_source_request_empty_enum PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME source_request_empty_enum COMMAND test_source_request_empty_enum)
set_tests_properties(source_request_empty_enum PROPERTIES TIMEOUT 120 LABELS "unit;api;source;ctfe;ownership")

add_executable(test_source_nullable_print "${CMAKE_CURRENT_LIST_DIR}/source_public/test_source_nullable_print.c")
target_link_libraries(test_source_nullable_print PRIVATE xray_source_api)
target_include_directories(test_source_nullable_print PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_definitions(test_source_nullable_print PRIVATE
    XR_PUBLIC_STDLIB="${PROJECT_SOURCE_DIR}/stdlib"
    XR_NULLABLE_PRINT_SOURCE="${PROJECT_SOURCE_DIR}/tests/fixtures/nullable_string_print/main.xr")
set(nullable_print_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/nullable_string_print.c")
add_custom_command(OUTPUT "${nullable_print_generated}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_source_nullable_print> --emit "${nullable_print_generated}"
    DEPENDS test_source_nullable_print "${PROJECT_SOURCE_DIR}/tests/fixtures/nullable_string_print/main.xr"
    VERBATIM)
add_executable(test_source_nullable_print_native
    "${CMAKE_CURRENT_LIST_DIR}/source_public/test_source_nullable_print_native.c" "${nullable_print_generated}")
target_link_libraries(test_source_nullable_print_native PRIVATE xray_xir_scalar xray_compile_resources)
foreach(target test_source_nullable_print test_source_nullable_print_native)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
        if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
            target_compile_options(${target} PRIVATE /experimental:c11atomics)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -pedantic)
    endif()
endforeach()
add_test(NAME source_nullable_string_print COMMAND test_source_nullable_print)
add_test(NAME source_nullable_string_print_native COMMAND test_source_nullable_print_native)
set_tests_properties(source_nullable_string_print source_nullable_string_print_native PROPERTIES
    TIMEOUT 60 LABELS "unit;api;source;ownership;nullable;generated-c")
