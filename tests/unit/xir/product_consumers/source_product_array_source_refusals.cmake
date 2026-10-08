add_executable(test_source_product_array_source_refusals "${CMAKE_CURRENT_LIST_DIR}/test_source_product_array_source_refusals.c")
target_link_libraries(test_source_product_array_source_refusals PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_array_source_refusals PRIVATE "${CMAKE_CURRENT_LIST_DIR}/..")
set_target_properties(test_source_product_array_source_refusals PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_product_array_source_refusals PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_source_product_array_source_refusals PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_source_product_array_source_refusals COMMAND test_source_product_array_source_refusals
    "${CMAKE_CURRENT_LIST_DIR}/array_source_refusals")
set_tests_properties(test_source_product_array_source_refusals PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;source-product;program-consumer;ownership;rejection")
