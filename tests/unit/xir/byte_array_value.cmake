add_executable(test_xir_byte_array_value xir/test_xir_byte_array_value.c)
target_include_directories(test_xir_byte_array_value PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(test_xir_byte_array_value PRIVATE xray_xir_scalar)
set_target_properties(test_xir_byte_array_value PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_byte_array_value PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_byte_array_value PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_byte_array_value COMMAND test_xir_byte_array_value)
set_tests_properties(test_xir_byte_array_value PROPERTIES
    TIMEOUT 60 LABELS "unit;xir;array;ownership;resources")
