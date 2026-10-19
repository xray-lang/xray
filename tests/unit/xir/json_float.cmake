add_executable(test_xir_json_float "${CMAKE_CURRENT_LIST_DIR}/test_xir_json_float.c")
target_link_libraries(test_xir_json_float PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
set(XR_JSON_FLOAT_WORK "${CMAKE_CURRENT_BINARY_DIR}/json-float-work")
file(MAKE_DIRECTORY "${XR_JSON_FLOAT_WORK}")
target_compile_definitions(test_xir_json_float PRIVATE XR_JSON_FLOAT_WORK="${XR_JSON_FLOAT_WORK}"
    XR_JSON_NUMBER_SOURCE="${PROJECT_SOURCE_DIR}/stdlib/json/number-decoding.xr"
    XR_JSON_FLOAT_SOURCE="${PROJECT_SOURCE_DIR}/stdlib/json/float-decoding.xr")
set_property(SOURCE "${CMAKE_CURRENT_LIST_DIR}/test_xir_json_float.c" APPEND PROPERTY OBJECT_DEPENDS
    "${PROJECT_SOURCE_DIR}/stdlib/json/number-decoding.xr;${PROJECT_SOURCE_DIR}/stdlib/json/float-decoding.xr")
set_target_properties(test_xir_json_float PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_json_float PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_xir_json_float PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_json_float COMMAND test_xir_json_float)
set_tests_properties(test_xir_json_float PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE LABELS "unit;xir;source;ctfe;json;library;ownership;numeric")
add_test(NAME test_xir_json_float_catalog_state COMMAND test_xir_json_float --catalog-state)
set_tests_properties(test_xir_json_float_catalog_state PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE
    LABELS "unit;xir;source;json;library;ownership;negative")
