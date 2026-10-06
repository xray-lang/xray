# Public definition admission complements the full named reader consumer.
set(semantic67_writer_target test_source_product_semantic67_public_writer)
add_executable(${semantic67_writer_target}
    "${CMAKE_CURRENT_LIST_DIR}/semantic67_named/test_source_product_semantic67_public_writer.c")
target_link_libraries(${semantic67_writer_target} PRIVATE xray_xir_source_product)
target_include_directories(${semantic67_writer_target} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/..")
set_target_properties(${semantic67_writer_target} PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(${semantic67_writer_target} PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(${semantic67_writer_target} PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_source_product_semantic67_public_writer COMMAND ${semantic67_writer_target})
set_tests_properties(test_source_product_semantic67_public_writer PROPERTIES
    TIMEOUT 120 LABELS "unit;xir;program-consumer;checked;ownership;named-packets;public-writer")
