add_executable(test_xir_json_aggregate_encode "${CMAKE_CURRENT_LIST_DIR}/test_xir_json_aggregate_encode.c")
target_link_libraries(test_xir_json_aggregate_encode PRIVATE xray_xir_source xray_xir_vm)
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
