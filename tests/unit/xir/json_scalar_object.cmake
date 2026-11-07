

set(XR_JSON_SCALAR_WORK "${CMAKE_CURRENT_BINARY_DIR}/json-integer_object-work")
file(MAKE_DIRECTORY "${XR_JSON_SCALAR_WORK}")

set_property(SOURCE "${CMAKE_CURRENT_LIST_DIR}/test_xir_json_scalar_object.c" APPEND PROPERTY OBJECT_DEPENDS
    "${PROJECT_SOURCE_DIR}/stdlib/json/number-decoding.xr"
    "${PROJECT_SOURCE_DIR}/stdlib/json/string-decoding.xr"
    "${PROJECT_SOURCE_DIR}/stdlib/json/integer-object-decoding.xr"
    "${PROJECT_SOURCE_DIR}/stdlib/json/scalar-object-decoding.xr")
