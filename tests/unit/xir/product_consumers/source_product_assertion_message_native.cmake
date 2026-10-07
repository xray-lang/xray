# Complete existing assertion source executes through generated native callbacks.
set(assertion_native_dir "${CMAKE_CURRENT_LIST_DIR}")
set(assertion_native_fixture "${CMAKE_BINARY_DIR}/generated/source-assertion-message")
set(assertion_native_root "${CMAKE_BINARY_DIR}/generated/source-assertion-message-native")
set(assertion_native_c "${assertion_native_root}/program.c")
file(MAKE_DIRECTORY "${assertion_native_root}")
add_executable(test_source_product_assertion_message_c_emitter
    "${assertion_native_dir}/test_source_product_assertion_message_native.c")
target_link_libraries(test_source_product_assertion_message_c_emitter PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_assertion_message_c_emitter PRIVATE "${assertion_native_dir}/..")
add_custom_command(OUTPUT "${assertion_native_c}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${assertion_native_root}"
    COMMAND $<TARGET_FILE:test_source_product_assertion_message_c_emitter>
        "${assertion_native_fixture}" "${assertion_native_fixture}/root.xr" --emit "${assertion_native_c}"
    DEPENDS test_source_product_assertion_message_c_emitter "${assertion_native_fixture}/root.xr"
        "${assertion_native_dir}/source_product_assertion_message.cmake"
    VERBATIM)
add_executable(test_source_product_assertion_message_native
    "${assertion_native_dir}/test_source_product_assertion_message_native.c" "${assertion_native_c}")
target_compile_definitions(test_source_product_assertion_message_native PRIVATE XR_SOURCE_ASSERTION_MESSAGE_NATIVE=1)
target_link_libraries(test_source_product_assertion_message_native PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_assertion_message_native PRIVATE "${assertion_native_dir}/..")
foreach(target IN ITEMS test_source_product_assertion_message_c_emitter test_source_product_assertion_message_native)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_source_product_assertion_message_native_normal COMMAND test_source_product_assertion_message_native
    "${assertion_native_fixture}" "${assertion_native_fixture}/root.xr")
set_tests_properties(test_source_product_assertion_message_native_normal PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;source-product;program-consumer;assertion;ownership;native-projection")
