# Timer layout and native C action mapping use the public source owner.
set(timer_cgen_dir "${CMAKE_CURRENT_LIST_DIR}")
add_executable(test_source_product_timer_cgen "${timer_cgen_dir}/test_source_product_timer_cgen.c")
target_link_libraries(test_source_product_timer_cgen PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_timer_cgen PRIVATE "${timer_cgen_dir}/..")
set_target_properties(test_source_product_timer_cgen PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_product_timer_cgen PRIVATE /utf-8 /W4 /WX)
    target_compile_definitions(test_source_product_timer_cgen PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(test_source_product_timer_cgen PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_source_product_timer_cgen
    COMMAND test_source_product_timer_cgen
        "${timer_cgen_dir}/fixtures/time_sleep"
        "${timer_cgen_dir}/fixtures/time_sleep/root.xr"
        "${CMAKE_SOURCE_DIR}/stdlib")
set_tests_properties(test_source_product_timer_cgen PROPERTIES TIMEOUT 120
    LABELS "unit;xir;source-product;program-consumer;ownership;metadata;timer-cgen")
