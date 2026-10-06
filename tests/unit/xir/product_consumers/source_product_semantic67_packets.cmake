# Full named packets retain original reader and private writer-only roles.
set(semantic67_named_target test_source_product_semantic67_named_packets)
add_executable(${semantic67_named_target}
    "${CMAKE_CURRENT_LIST_DIR}/semantic67_named/test_source_product_semantic67_named_packets.c")
target_link_libraries(${semantic67_named_target} PRIVATE xray_xir_source_product)
target_include_directories(${semantic67_named_target} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/..")
set_target_properties(${semantic67_named_target} PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(${semantic67_named_target} PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(${semantic67_named_target} PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_source_product_semantic67_named_packets COMMAND ${semantic67_named_target})
set_tests_properties(test_source_product_semantic67_named_packets PROPERTIES
    TIMEOUT 120 LABELS "unit;xir;program-consumer;checked;ownership;named-packets")
