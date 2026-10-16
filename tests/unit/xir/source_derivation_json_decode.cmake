add_executable(test_xir_source_derivation_json_decode "${CMAKE_CURRENT_LIST_DIR}/test_xir_source_derivation_json_decode.c")
target_link_libraries(test_xir_source_derivation_json_decode PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
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
set(XR_JSON_DECODE_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_json_decode.c")
set(XR_JSON_DECODE_PACKET "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_json_decode.chk")
add_custom_command(OUTPUT ${XR_JSON_DECODE_C} ${XR_JSON_DECODE_PACKET}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_source_derivation_json_decode> --emit-native ${XR_JSON_DECODE_C} ${XR_JSON_DECODE_PACKET}
    DEPENDS test_xir_source_derivation_json_decode VERBATIM)
add_executable(test_xir_source_derivation_json_decode_native "${CMAKE_CURRENT_LIST_DIR}/test_xir_source_derivation_json_decode_native.c" ${XR_JSON_DECODE_C})
target_link_libraries(test_xir_source_derivation_json_decode_native PRIVATE xray_xir_scalar)
add_executable(test_xir_source_derivation_json_decode_mixed "${CMAKE_CURRENT_LIST_DIR}/test_xir_source_derivation_json_decode_mixed.c" ${XR_JSON_DECODE_C})
target_link_libraries(test_xir_source_derivation_json_decode_mixed PRIVATE xray_xir_vm)
target_compile_definitions(test_xir_source_derivation_json_decode_mixed PRIVATE XR_JSON_DECODE_PACKET="${XR_JSON_DECODE_PACKET}")
foreach(decoder_test IN ITEMS test_xir_source_derivation_json_decode_native test_xir_source_derivation_json_decode_mixed)
    set_target_properties(${decoder_test} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${decoder_test} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${decoder_test} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${decoder_test} COMMAND ${decoder_test})
    set_tests_properties(${decoder_test} PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE
        LABELS "unit;xir;source;ctfe;generated;library;json;ownership;authorization;execution;abi")
endforeach()
foreach(decoder_test IN ITEMS test_xir_source_derivation_json_decode_native test_xir_source_derivation_json_decode_mixed)
    add_test(NAME ${decoder_test}_runtime_resources COMMAND ${decoder_test} --runtime-resources)
    set_tests_properties(${decoder_test}_runtime_resources PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE
        LABELS "unit;xir;source;ctfe;generated;json;ownership;runtime-fault;resources")
endforeach()
add_test(NAME test_xir_source_derivation_json_decode_runtime_resources COMMAND test_xir_source_derivation_json_decode_mixed --vm-runtime-resources)
set_tests_properties(test_xir_source_derivation_json_decode_runtime_resources PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE
    LABELS "unit;xir;source;ctfe;generated;json;ownership;runtime-fault;resources")
