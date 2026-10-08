add_executable(test_source_product_module_class_error "${CMAKE_CURRENT_LIST_DIR}/test_source_product_module_class_error.c")
target_link_libraries(test_source_product_module_class_error PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_module_class_error PRIVATE "${CMAKE_CURRENT_LIST_DIR}/..")
set_target_properties(test_source_product_module_class_error PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_product_module_class_error PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_source_product_module_class_error PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_source_product_module_class_error COMMAND test_source_product_module_class_error)
set_tests_properties(test_source_product_module_class_error PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;program-consumer;module-graph;ownership")
add_test(NAME source_product_module_class_error_encoding
    COMMAND ${XRAY_PYTHON} -B -X utf8 "${CMAKE_SOURCE_DIR}/scripts/source_product_consumer_module_class_error.py" check)
set_tests_properties(source_product_module_class_error_encoding PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;program-consumer;inventory")
