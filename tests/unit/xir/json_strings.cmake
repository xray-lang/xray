add_executable(test_xir_json_strings "${CMAKE_CURRENT_LIST_DIR}/test_xir_json_strings.c")
target_link_libraries(test_xir_json_strings PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
set(XR_JSON_STRINGS_WORK "${CMAKE_CURRENT_BINARY_DIR}/json-strings-work")
file(MAKE_DIRECTORY "${XR_JSON_STRINGS_WORK}")
target_compile_definitions(test_xir_json_strings PRIVATE
    XR_JSON_STRINGS_WORK="${XR_JSON_STRINGS_WORK}"
    XR_JSON_ENCODING_SOURCE="${PROJECT_SOURCE_DIR}/stdlib/json/encoding.xr"
    XR_JSON_DECODING_SOURCE="${PROJECT_SOURCE_DIR}/stdlib/json/string-decoding.xr")
set_property(SOURCE "${CMAKE_CURRENT_LIST_DIR}/test_xir_json_strings.c" APPEND PROPERTY OBJECT_DEPENDS
    "${PROJECT_SOURCE_DIR}/stdlib/json/encoding.xr;${PROJECT_SOURCE_DIR}/stdlib/json/string-decoding.xr")
set(XR_JSON_STRINGS_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_json_strings.c")
set(XR_JSON_STRINGS_PACKET "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_json_strings.chk")
add_custom_command(OUTPUT ${XR_JSON_STRINGS_C} ${XR_JSON_STRINGS_PACKET}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_json_strings> --emit-native ${XR_JSON_STRINGS_C} ${XR_JSON_STRINGS_PACKET}
    DEPENDS test_xir_json_strings VERBATIM)
add_executable(test_xir_json_strings_native "${CMAKE_CURRENT_LIST_DIR}/test_xir_json_strings_native.c" ${XR_JSON_STRINGS_C})
target_link_libraries(test_xir_json_strings_native PRIVATE xray_xir_scalar)
add_executable(test_xir_json_strings_mixed "${CMAKE_CURRENT_LIST_DIR}/test_xir_json_strings_mixed.c" ${XR_JSON_STRINGS_C})
target_link_libraries(test_xir_json_strings_mixed PRIVATE xray_xir_vm)
target_compile_definitions(test_xir_json_strings_mixed PRIVATE XR_JSON_STRINGS_PACKET="${XR_JSON_STRINGS_PACKET}")
foreach(strings_test IN ITEMS test_xir_json_strings test_xir_json_strings_native test_xir_json_strings_mixed)
    set_target_properties(${strings_test} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${strings_test} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${strings_test} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${strings_test} COMMAND ${strings_test})
    set_tests_properties(${strings_test} PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE LABELS "unit;xir;source;ctfe;json;library;ownership;execution;abi")
endforeach()
foreach(strings_test IN ITEMS test_xir_json_strings_native test_xir_json_strings_mixed)
    add_test(NAME ${strings_test}_runtime_resources COMMAND ${strings_test} --runtime-resources)
    set_tests_properties(${strings_test}_runtime_resources PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE
        LABELS "unit;xir;source;json;ownership;runtime-fault;resources")
endforeach()
add_test(NAME test_xir_json_strings_runtime_resources COMMAND test_xir_json_strings_mixed --vm-runtime-resources)
set_tests_properties(test_xir_json_strings_runtime_resources PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE
    LABELS "unit;xir;source;json;ownership;runtime-fault;resources")
