add_executable(test_original_time_sleep_source
    "${CMAKE_CURRENT_LIST_DIR}/test_original_time_sleep_source.c")
target_link_libraries(test_original_time_sleep_source PRIVATE xray_xir_source_product)
target_include_directories(test_original_time_sleep_source PRIVATE "${CMAKE_CURRENT_LIST_DIR}/..")
set_target_properties(test_original_time_sleep_source PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_original_time_sleep_source PRIVATE /utf-8 /W4 /WX)
    target_compile_definitions(test_original_time_sleep_source PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(test_original_time_sleep_source PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_original_time_sleep_source COMMAND test_original_time_sleep_source
    "${CMAKE_CURRENT_LIST_DIR}/original_time_sleep" "${CMAKE_CURRENT_LIST_DIR}/original_time_sleep/root.xr"
    "${CMAKE_SOURCE_DIR}/stdlib")
set_tests_properties(test_original_time_sleep_source PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;source-product;program-consumer;time;suspension;ownership;original-source")
