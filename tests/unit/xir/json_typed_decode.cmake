add_executable(test_xir_json_typed_decode "${CMAKE_CURRENT_LIST_DIR}/test_xir_json_typed_decode.c")
target_link_libraries(test_xir_json_typed_decode PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
set(XR_JSON_TYPED_WORK "${CMAKE_CURRENT_BINARY_DIR}/json-typed-work")
file(MAKE_DIRECTORY "${XR_JSON_TYPED_WORK}")
target_compile_definitions(test_xir_json_typed_decode PRIVATE
    XR_JSON_TYPED_WORK="${XR_JSON_TYPED_WORK}"
    XR_JSON_NUMBER_SOURCE="${PROJECT_SOURCE_DIR}/stdlib/json/number-decoding.xr"
    XR_JSON_STRING_SOURCE="${PROJECT_SOURCE_DIR}/stdlib/json/string-decoding.xr"
    XR_JSON_OBJECT_SOURCE="${PROJECT_SOURCE_DIR}/stdlib/json/integer-object-decoding.xr"
    XR_JSON_SCALAR_SOURCE="${PROJECT_SOURCE_DIR}/stdlib/json/scalar-object-decoding.xr")
set_property(SOURCE "${CMAKE_CURRENT_LIST_DIR}/test_xir_json_typed_decode.c" APPEND PROPERTY OBJECT_DEPENDS
    "${PROJECT_SOURCE_DIR}/stdlib/json/number-decoding.xr"
    "${PROJECT_SOURCE_DIR}/stdlib/json/string-decoding.xr"
    "${PROJECT_SOURCE_DIR}/stdlib/json/integer-object-decoding.xr"
    "${PROJECT_SOURCE_DIR}/stdlib/json/scalar-object-decoding.xr")
set_target_properties(test_xir_json_typed_decode PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_json_typed_decode PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_xir_json_typed_decode PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_json_typed_decode COMMAND test_xir_json_typed_decode)
set_tests_properties(test_xir_json_typed_decode PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE
    LABELS "unit;xir;source;ctfe;generated;json;library;authorization;ownership")
set(XR_JSON_TYPED_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_json_typed.c")
set(XR_JSON_TYPED_PACKET "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_json_typed.chk")
add_custom_command(OUTPUT ${XR_JSON_TYPED_C} ${XR_JSON_TYPED_PACKET}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_json_typed_decode> --emit-native ${XR_JSON_TYPED_C} ${XR_JSON_TYPED_PACKET}
    DEPENDS test_xir_json_typed_decode VERBATIM)
add_executable(test_xir_json_typed_decode_native "${CMAKE_CURRENT_LIST_DIR}/test_xir_json_typed_decode_native.c" ${XR_JSON_TYPED_C})
target_link_libraries(test_xir_json_typed_decode_native PRIVATE xray_xir_scalar)
add_executable(test_xir_json_typed_decode_mixed "${CMAKE_CURRENT_LIST_DIR}/test_xir_json_typed_decode_mixed.c" ${XR_JSON_TYPED_C})
target_link_libraries(test_xir_json_typed_decode_mixed PRIVATE xray_xir_vm)
target_compile_definitions(test_xir_json_typed_decode_mixed PRIVATE XR_JSON_TYPED_PACKET="${XR_JSON_TYPED_PACKET}")
foreach(named_test IN ITEMS test_xir_json_typed_decode_native test_xir_json_typed_decode_mixed)
    set_target_properties(${named_test} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${named_test} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${named_test} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${named_test} COMMAND ${named_test})
    set_tests_properties(${named_test} PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE
        LABELS "unit;xir;source;ctfe;generated;json;library;authorization;ownership;abi;execution")
endforeach()

