add_executable(test_source_product_nominal_names "${CMAKE_CURRENT_LIST_DIR}/test_source_product_nominal_names.c")
target_link_libraries(test_source_product_nominal_names PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_nominal_names PRIVATE "${CMAKE_CURRENT_LIST_DIR}/..")
set_target_properties(test_source_product_nominal_names PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_product_nominal_names PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_source_product_nominal_names PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_source_product_nominal_names COMMAND test_source_product_nominal_names)
set_tests_properties(test_source_product_nominal_names PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;program-consumer;nominal;ownership")
add_test(NAME source_product_nominal_name_encoding
    COMMAND ${XRAY_PYTHON} -B -X utf8 "${CMAKE_SOURCE_DIR}/scripts/source_product_consumer_nominal_names.py" check)
set_tests_properties(source_product_nominal_name_encoding PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;program-consumer;inventory")
