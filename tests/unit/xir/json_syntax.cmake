add_executable(test_xir_json_syntax "${CMAKE_CURRENT_LIST_DIR}/test_xir_json_syntax.c")
target_link_libraries(test_xir_json_syntax PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
set(XR_JSON_SYNTAX_WORK "${CMAKE_CURRENT_BINARY_DIR}/json-syntax-work")
file(MAKE_DIRECTORY "${XR_JSON_SYNTAX_WORK}")
target_compile_definitions(test_xir_json_syntax PRIVATE
    XR_JSON_SYNTAX_WORK="${XR_JSON_SYNTAX_WORK}"
    XR_JSON_STRING_SOURCE="${PROJECT_SOURCE_DIR}/stdlib/json/string-decoding.xr"
    XR_JSON_NUMBER_SOURCE="${PROJECT_SOURCE_DIR}/stdlib/json/number-decoding.xr"
    XR_JSON_SYNTAX_SOURCE="${PROJECT_SOURCE_DIR}/stdlib/json/syntax.xr")
set_property(SOURCE "${CMAKE_CURRENT_LIST_DIR}/test_xir_json_syntax.c" APPEND PROPERTY OBJECT_DEPENDS
    "${PROJECT_SOURCE_DIR}/stdlib/json/string-decoding.xr;${PROJECT_SOURCE_DIR}/stdlib/json/number-decoding.xr;${PROJECT_SOURCE_DIR}/stdlib/json/syntax.xr")
set(XR_JSON_SYNTAX_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_json_syntax.c")
set(XR_JSON_SYNTAX_PACKET "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_json_syntax.chk")
add_custom_command(OUTPUT ${XR_JSON_SYNTAX_C} ${XR_JSON_SYNTAX_PACKET}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_json_syntax> --emit-native ${XR_JSON_SYNTAX_C} ${XR_JSON_SYNTAX_PACKET}
    DEPENDS test_xir_json_syntax VERBATIM)
add_executable(test_xir_json_syntax_native "${CMAKE_CURRENT_LIST_DIR}/test_xir_json_syntax_native.c" ${XR_JSON_SYNTAX_C})
target_link_libraries(test_xir_json_syntax_native PRIVATE xray_xir_scalar)
add_executable(test_xir_json_syntax_mixed "${CMAKE_CURRENT_LIST_DIR}/test_xir_json_syntax_mixed.c" ${XR_JSON_SYNTAX_C})
target_link_libraries(test_xir_json_syntax_mixed PRIVATE xray_xir_vm)
target_compile_definitions(test_xir_json_syntax_mixed PRIVATE XR_JSON_SYNTAX_PACKET="${XR_JSON_SYNTAX_PACKET}")
foreach(syntax_test IN ITEMS test_xir_json_syntax test_xir_json_syntax_native test_xir_json_syntax_mixed)
    set_target_properties(${syntax_test} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${syntax_test} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${syntax_test} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${syntax_test} COMMAND ${syntax_test})
    set_tests_properties(${syntax_test} PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE LABELS "unit;xir;source;ctfe;json;library;ownership;execution;abi")
endforeach()
