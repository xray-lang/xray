add_executable(test_source_product_checked_hostile_frame
    "${CMAKE_CURRENT_LIST_DIR}/test_source_product_checked_hostile_frame.c")
target_link_libraries(test_source_product_checked_hostile_frame PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_checked_hostile_frame PRIVATE "${CMAKE_CURRENT_LIST_DIR}/..")
set_target_properties(test_source_product_checked_hostile_frame PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_product_checked_hostile_frame PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_source_product_checked_hostile_frame PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_source_product_checked_hostile_frame COMMAND test_source_product_checked_hostile_frame)
set_tests_properties(test_source_product_checked_hostile_frame PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;program-consumer;checked;ownership;framing")
add_test(NAME source_product_checked_semantic_fixture_encoding
    COMMAND ${XRAY_PYTHON} -B -X utf8 "${CMAKE_SOURCE_DIR}/scripts/source_product_consumer_checked_semantics.py" check)
set_tests_properties(source_product_checked_semantic_fixture_encoding PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;program-consumer;checked;inventory")
