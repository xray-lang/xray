add_executable(test_xir_json_numbers "${CMAKE_CURRENT_LIST_DIR}/test_xir_json_numbers.c")
target_link_libraries(test_xir_json_numbers PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
set(XR_JSON_NUMBERS_WORK "${CMAKE_CURRENT_BINARY_DIR}/json-numbers-work")
file(MAKE_DIRECTORY "${XR_JSON_NUMBERS_WORK}")
target_compile_definitions(test_xir_json_numbers PRIVATE
    XR_JSON_NUMBERS_WORK="${XR_JSON_NUMBERS_WORK}"
    XR_JSON_NUMBER_SOURCE="${PROJECT_SOURCE_DIR}/stdlib/json/number-decoding.xr")
set_property(SOURCE "${CMAKE_CURRENT_LIST_DIR}/test_xir_json_numbers.c" APPEND PROPERTY OBJECT_DEPENDS
    "${PROJECT_SOURCE_DIR}/stdlib/json/number-decoding.xr")
set(XR_JSON_NUMBERS_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_json_numbers.c")
set(XR_JSON_NUMBERS_PACKET "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_json_numbers.chk")
add_custom_command(OUTPUT ${XR_JSON_NUMBERS_C} ${XR_JSON_NUMBERS_PACKET}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_json_numbers> --emit-native ${XR_JSON_NUMBERS_C} ${XR_JSON_NUMBERS_PACKET}
    DEPENDS test_xir_json_numbers VERBATIM)
add_executable(test_xir_json_numbers_native "${CMAKE_CURRENT_LIST_DIR}/test_xir_json_numbers_native.c" ${XR_JSON_NUMBERS_C})
target_link_libraries(test_xir_json_numbers_native PRIVATE xray_xir_scalar)
add_executable(test_xir_json_numbers_mixed "${CMAKE_CURRENT_LIST_DIR}/test_xir_json_numbers_mixed.c" ${XR_JSON_NUMBERS_C})
target_link_libraries(test_xir_json_numbers_mixed PRIVATE xray_xir_vm)
target_compile_definitions(test_xir_json_numbers_mixed PRIVATE XR_JSON_NUMBERS_PACKET="${XR_JSON_NUMBERS_PACKET}")
foreach(numbers_test IN ITEMS test_xir_json_numbers test_xir_json_numbers_native test_xir_json_numbers_mixed)
    set_target_properties(${numbers_test} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${numbers_test} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${numbers_test} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${numbers_test} COMMAND ${numbers_test})
    set_tests_properties(${numbers_test} PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE LABELS "unit;xir;source;ctfe;json;library;ownership;execution;abi")
endforeach()
foreach(numbers_test IN ITEMS test_xir_json_numbers_native test_xir_json_numbers_mixed)
    add_test(NAME ${numbers_test}_runtime_resources COMMAND ${numbers_test} --runtime-resources)
    set_tests_properties(${numbers_test}_runtime_resources PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE
        LABELS "unit;xir;source;json;ownership;runtime-fault;resources")
endforeach()
add_test(NAME test_xir_json_numbers_runtime_resources COMMAND test_xir_json_numbers_mixed --vm-runtime-resources)
set_tests_properties(test_xir_json_numbers_runtime_resources PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE
    LABELS "unit;xir;source;json;ownership;runtime-fault;resources")
