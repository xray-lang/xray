add_executable(test_xir_source_derivation_json_decode "${CMAKE_CURRENT_LIST_DIR}/test_xir_source_derivation_json_decode.c")
target_link_libraries(test_xir_source_derivation_json_decode PRIVATE xray_xir_source xray_xir_vm)
set(XR_JSON_DECODE_WORK "${CMAKE_CURRENT_BINARY_DIR}/source-json-decode-work")
file(MAKE_DIRECTORY "${XR_JSON_DECODE_WORK}")
target_compile_definitions(test_xir_source_derivation_json_decode PRIVATE
    XR_JSON_DECODE_WORK="${XR_JSON_DECODE_WORK}" XR_JSON_NUMBER_SOURCE="${PROJECT_SOURCE_DIR}/stdlib/json/number-decoding.xr")
set_property(SOURCE "${CMAKE_CURRENT_LIST_DIR}/test_xir_source_derivation_json_decode.c" APPEND PROPERTY OBJECT_DEPENDS
    "${PROJECT_SOURCE_DIR}/stdlib/json/number-decoding.xr")
set_target_properties(test_xir_source_derivation_json_decode PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_source_derivation_json_decode PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_xir_source_derivation_json_decode PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_source_derivation_json_decode COMMAND test_xir_source_derivation_json_decode)
set_tests_properties(test_xir_source_derivation_json_decode PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE
    LABELS "unit;xir;source;ctfe;generated;library;json;ownership;authorization")
