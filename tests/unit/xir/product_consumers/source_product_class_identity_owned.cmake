add_executable(test_source_product_class_identity_owned "${CMAKE_CURRENT_LIST_DIR}/test_source_product_class_identity_owned.c")
target_link_libraries(test_source_product_class_identity_owned PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_class_identity_owned PRIVATE "${CMAKE_CURRENT_LIST_DIR}/..")
set_target_properties(test_source_product_class_identity_owned PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_product_class_identity_owned PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_source_product_class_identity_owned PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_source_product_class_identity_owned COMMAND test_source_product_class_identity_owned
    "${product_consumer_fixture_root}/class_identity" "${product_consumer_fixture_root}/class_identity/root.xr" "${CMAKE_SOURCE_DIR}/stdlib")
set_tests_properties(test_source_product_class_identity_owned PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;program-consumer;class;array;ownership;source")
