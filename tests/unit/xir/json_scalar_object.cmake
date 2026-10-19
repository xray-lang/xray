add_executable(test_xir_json_scalar_object "${CMAKE_CURRENT_LIST_DIR}/test_xir_json_scalar_object.c")
target_link_libraries(test_xir_json_scalar_object PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
set(XR_JSON_SCALAR_WORK "${CMAKE_CURRENT_BINARY_DIR}/json-integer_object-work")
file(MAKE_DIRECTORY "${XR_JSON_SCALAR_WORK}")
target_compile_definitions(test_xir_json_scalar_object PRIVATE
    XR_JSON_SCALAR_WORK="${XR_JSON_SCALAR_WORK}"
    XR_JSON_NUMBER_SOURCE="${PROJECT_SOURCE_DIR}/stdlib/json/number-decoding.xr"
    XR_JSON_STRING_SOURCE="${PROJECT_SOURCE_DIR}/stdlib/json/string-decoding.xr"
    XR_JSON_INTEGER_OBJECT_SOURCE="${PROJECT_SOURCE_DIR}/stdlib/json/integer-object-decoding.xr"
    XR_JSON_SCALAR_SOURCE="${PROJECT_SOURCE_DIR}/stdlib/json/scalar-object-decoding.xr")
set_property(SOURCE "${CMAKE_CURRENT_LIST_DIR}/test_xir_json_scalar_object.c" APPEND PROPERTY OBJECT_DEPENDS
    "${PROJECT_SOURCE_DIR}/stdlib/json/number-decoding.xr"
    "${PROJECT_SOURCE_DIR}/stdlib/json/string-decoding.xr"
    "${PROJECT_SOURCE_DIR}/stdlib/json/integer-object-decoding.xr"
    "${PROJECT_SOURCE_DIR}/stdlib/json/scalar-object-decoding.xr")
set(XR_JSON_SCALAR_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_json_scalar_object.c")
set(XR_JSON_SCALAR_PACKET "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_json_scalar_object.chk")
add_custom_command(OUTPUT ${XR_JSON_SCALAR_C} ${XR_JSON_SCALAR_PACKET}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_json_scalar_object> --emit-native ${XR_JSON_SCALAR_C} ${XR_JSON_SCALAR_PACKET}
    DEPENDS test_xir_json_scalar_object VERBATIM)
add_executable(test_xir_json_scalar_object_native "${CMAKE_CURRENT_LIST_DIR}/test_xir_json_scalar_object_native.c" ${XR_JSON_SCALAR_C})
target_link_libraries(test_xir_json_scalar_object_native PRIVATE xray_xir_scalar)
add_executable(test_xir_json_scalar_object_mixed "${CMAKE_CURRENT_LIST_DIR}/test_xir_json_scalar_object_mixed.c" ${XR_JSON_SCALAR_C})
target_link_libraries(test_xir_json_scalar_object_mixed PRIVATE xray_xir_vm)
target_compile_definitions(test_xir_json_scalar_object_mixed PRIVATE XR_JSON_SCALAR_PACKET="${XR_JSON_SCALAR_PACKET}")
foreach(integer_object_test IN ITEMS test_xir_json_scalar_object test_xir_json_scalar_object_native test_xir_json_scalar_object_mixed)
    set_target_properties(${integer_object_test} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${integer_object_test} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${integer_object_test} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${integer_object_test} COMMAND ${integer_object_test})
    set_tests_properties(${integer_object_test} PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE LABELS "unit;xir;source;ctfe;json;library;ownership;execution;abi")
endforeach()
