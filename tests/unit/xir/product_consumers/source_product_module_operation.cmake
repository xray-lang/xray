add_executable(test_source_product_module_operation "${CMAKE_CURRENT_LIST_DIR}/test_source_product_module_operation.c")
target_link_libraries(test_source_product_module_operation PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_module_operation PRIVATE "${CMAKE_CURRENT_LIST_DIR}/..")
set_target_properties(test_source_product_module_operation PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_product_module_operation PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_source_product_module_operation PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_source_product_module_operation COMMAND test_source_product_module_operation)
set_tests_properties(test_source_product_module_operation PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;program-consumer;module-graph;ownership")
add_test(NAME source_product_module_operation_encoding
    COMMAND ${XRAY_PYTHON} -B -X utf8 "${CMAKE_SOURCE_DIR}/scripts/source_product_consumer_module_operation.py" check)
set_tests_properties(source_product_module_operation_encoding PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;program-consumer;inventory")
