add_executable(test_source_product_construction_v2_integer_admission
    "${CMAKE_CURRENT_LIST_DIR}/test_source_product_construction_v2_integer_admission.c")
target_link_libraries(test_source_product_construction_v2_integer_admission PRIVATE xray_xir_admission)
target_include_directories(test_source_product_construction_v2_integer_admission PRIVATE "${CMAKE_CURRENT_LIST_DIR}/..")
set_target_properties(test_source_product_construction_v2_integer_admission PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_product_construction_v2_integer_admission PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_source_product_construction_v2_integer_admission PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_source_product_construction_v2_integer_admission
    COMMAND test_source_product_construction_v2_integer_admission)
set_tests_properties(test_source_product_construction_v2_integer_admission PROPERTIES
    TIMEOUT 120 PROCESSORS 1 LABELS "unit;xir;program-consumer;integer;construction;ownership")
