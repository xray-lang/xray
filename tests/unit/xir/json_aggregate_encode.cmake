add_executable(test_xir_json_aggregate_encode "${CMAKE_CURRENT_LIST_DIR}/test_xir_json_aggregate_encode.c")
target_link_libraries(test_xir_json_aggregate_encode PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
set(XR_JSON_AGGREGATE_WORK "${CMAKE_CURRENT_BINARY_DIR}/json-aggregate-encode-work")
file(MAKE_DIRECTORY "${XR_JSON_AGGREGATE_WORK}")
target_compile_definitions(test_xir_json_aggregate_encode PRIVATE XR_JSON_AGGREGATE_WORK="${XR_JSON_AGGREGATE_WORK}"
    XR_JSON_AGGREGATE_SOURCE="${PROJECT_SOURCE_DIR}/stdlib/json/aggregate-encoding.xr")
set_property(SOURCE "${CMAKE_CURRENT_LIST_DIR}/test_xir_json_aggregate_encode.c" APPEND PROPERTY OBJECT_DEPENDS
    "${PROJECT_SOURCE_DIR}/stdlib/json/aggregate-encoding.xr")
set_target_properties(test_xir_json_aggregate_encode PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_json_aggregate_encode PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_xir_json_aggregate_encode PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_json_aggregate_encode COMMAND test_xir_json_aggregate_encode)
set_tests_properties(test_xir_json_aggregate_encode PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE
    LABELS "unit;xir;source;ctfe;generated;json;library;ownership")
add_test(NAME test_xir_json_aggregate_encode_negative COMMAND test_xir_json_aggregate_encode --negative)
set_tests_properties(test_xir_json_aggregate_encode_negative PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE
    LABELS "unit;xir;source;ctfe;generated;json;library;ownership;negative")
set(aggregate_c "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_json_aggregate.c")
set(aggregate_packet "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_json_aggregate.chk")
add_custom_command(OUTPUT ${aggregate_c} ${aggregate_packet}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_json_aggregate_encode> --emit-native ${aggregate_c} ${aggregate_packet}
    DEPENDS test_xir_json_aggregate_encode VERBATIM)
foreach(aggregate_engine IN ITEMS native mixed)
    set(aggregate_target "test_xir_json_aggregate_encode_${aggregate_engine}")
    add_executable(${aggregate_target} "${CMAKE_CURRENT_LIST_DIR}/${aggregate_target}.c" ${aggregate_c})
    if(aggregate_engine STREQUAL "native")
        target_link_libraries(${aggregate_target} PRIVATE xray_xir_scalar)
    else()
        target_link_libraries(${aggregate_target} PRIVATE xray_xir_vm)
        target_compile_definitions(${aggregate_target} PRIVATE XR_JSON_AGGREGATE_PACKET="${aggregate_packet}")
    endif()
    set_target_properties(${aggregate_target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${aggregate_target} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${aggregate_target} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${aggregate_target} COMMAND ${aggregate_target})
    set_tests_properties(${aggregate_target} PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE
        LABELS "unit;xir;source;ctfe;generated;json;library;ownership;execution;abi")
    add_test(NAME "${aggregate_target}_runtime_resources" COMMAND ${aggregate_target} --runtime-resources)
    set_tests_properties("${aggregate_target}_runtime_resources" PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE
        LABELS "unit;xir;source;generated;json;library;ownership;runtime-fault;resources")
endforeach()
add_test(NAME test_xir_json_aggregate_encode_vm_runtime_resources COMMAND test_xir_json_aggregate_encode_mixed --vm-runtime-resources)
set_tests_properties(test_xir_json_aggregate_encode_vm_runtime_resources PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE
    LABELS "unit;xir;source;generated;json;library;ownership;runtime-fault;resources")
