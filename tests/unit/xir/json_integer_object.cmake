

set(XR_JSON_OBJECT_WORK "${CMAKE_CURRENT_BINARY_DIR}/json-integer_object-work")
file(MAKE_DIRECTORY "${XR_JSON_OBJECT_WORK}")

set_property(SOURCE "${CMAKE_CURRENT_LIST_DIR}/test_xir_json_integer_object.c" APPEND PROPERTY OBJECT_DEPENDS
    "${PROJECT_SOURCE_DIR}/stdlib/json/number-decoding.xr"
    "${PROJECT_SOURCE_DIR}/stdlib/json/string-decoding.xr"
    "${PROJECT_SOURCE_DIR}/stdlib/json/integer-object-decoding.xr")
set(XR_JSON_OBJECT_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_json_integer_object.c")
set(XR_JSON_OBJECT_PACKET "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_json_integer_object.chk")
