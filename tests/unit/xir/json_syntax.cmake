

set(XR_JSON_SYNTAX_WORK "${CMAKE_CURRENT_BINARY_DIR}/json-syntax-work")
file(MAKE_DIRECTORY "${XR_JSON_SYNTAX_WORK}")

set_property(SOURCE "${CMAKE_CURRENT_LIST_DIR}/test_xir_json_syntax.c" APPEND PROPERTY OBJECT_DEPENDS
    "${PROJECT_SOURCE_DIR}/stdlib/json/string-decoding.xr;${PROJECT_SOURCE_DIR}/stdlib/json/number-decoding.xr;${PROJECT_SOURCE_DIR}/stdlib/json/syntax.xr")
set(XR_JSON_SYNTAX_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_json_syntax.c")
set(XR_JSON_SYNTAX_PACKET "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_json_syntax.chk")
