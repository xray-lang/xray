add_executable(test_source_product_array_element_place_frames "${CMAKE_CURRENT_LIST_DIR}/test_source_product_array_element_place_frames.c")
target_link_libraries(test_source_product_array_element_place_frames PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_array_element_place_frames PRIVATE "${CMAKE_CURRENT_LIST_DIR}/..")
set_target_properties(test_source_product_array_element_place_frames PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_product_array_element_place_frames PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_source_product_array_element_place_frames PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_source_product_array_element_place_frames COMMAND test_source_product_array_element_place_frames)
set_tests_properties(test_source_product_array_element_place_frames PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;program-consumer;array;ownership;format")
add_test(NAME source_product_element_place_frame_encoding
    COMMAND ${XRAY_PYTHON} -B -X utf8 "${CMAKE_SOURCE_DIR}/scripts/source_product_consumer_element_place_frames.py" check)
set_tests_properties(source_product_element_place_frame_encoding PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;program-consumer;inventory")
