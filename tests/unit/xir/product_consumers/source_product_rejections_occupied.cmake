add_executable(test_source_product_rejections_occupied "${CMAKE_CURRENT_LIST_DIR}/test_source_product_rejections_occupied.c")
target_link_libraries(test_source_product_rejections_occupied PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_rejections_occupied PRIVATE "${CMAKE_CURRENT_LIST_DIR}/..")
set_target_properties(test_source_product_rejections_occupied PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_product_rejections_occupied PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_source_product_rejections_occupied PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_source_product_rejections_occupied COMMAND test_source_product_rejections_occupied
    "${CMAKE_CURRENT_LIST_DIR}/fixtures")
set_tests_properties(test_source_product_rejections_occupied PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;source-product;program-consumer;ownership;rejection")
