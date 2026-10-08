add_executable(test_source_product_channel_owner "${CMAKE_CURRENT_LIST_DIR}/test_source_product_channel_owner.c")
target_link_libraries(test_source_product_channel_owner PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_channel_owner PRIVATE "${CMAKE_CURRENT_LIST_DIR}/..")
set_target_properties(test_source_product_channel_owner PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_product_channel_owner PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_source_product_channel_owner PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_source_product_channel_owner COMMAND test_source_product_channel_owner
    "${CMAKE_CURRENT_LIST_DIR}/channel_admission" "${CMAKE_SOURCE_DIR}/stdlib"
    "${CMAKE_CURRENT_LIST_DIR}/parent_source/open_root_empty")
set_tests_properties(test_source_product_channel_owner PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;program-consumer;channel;ownership")
add_test(NAME source_product_channel_owner_inputs
    COMMAND ${XRAY_PYTHON} -B -X utf8 "${CMAKE_SOURCE_DIR}/scripts/source_product_consumer_channel_owner.py" check)
set_tests_properties(source_product_channel_owner_inputs PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;program-consumer;inventory")
