add_executable(test_xir_effect_final_read ${CMAKE_CURRENT_LIST_DIR}/test_xir_root_conditional_helpers.c)
target_include_directories(test_xir_effect_final_read PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(test_xir_effect_final_read PRIVATE xray_xir xray_xir_scalar xray_xir_source)
if(MSVC)
    target_compile_options(test_xir_effect_final_read PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_effect_final_read PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_effect_final_read_allocation_bounds
    COMMAND test_xir_effect_final_read --read-allocation-bounds)
set_tests_properties(test_xir_effect_final_read_allocation_bounds PROPERTIES
    LABELS "unit;xir;root-effects;ownership;metadata;final-read"
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1)

add_test(NAME test_xir_effect_final_recursive_trace
    COMMAND test_xir_effect_final_read --rotation)
set_tests_properties(test_xir_effect_final_recursive_trace PROPERTIES
    LABELS "unit;xir;root-effects;ownership;metadata;final-read;recursion"
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1)

add_test(NAME test_xir_effect_current_input_callback
    COMMAND test_xir_error_cfg_pending --current-input-callback)
set_tests_properties(test_xir_effect_current_input_callback PROPERTIES
    LABELS "unit;xir;root-effects;ownership;metadata;final-input;callback"
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1)

add_test(NAME test_xir_effect_current_input_resources
    COMMAND test_xir_error_cfg_pending --current-input-resources)
set_tests_properties(test_xir_effect_current_input_resources PROPERTIES
    LABELS "unit;xir;root-effects;ownership;metadata;final-input;callback;resources"
    TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)

add_test(NAME test_xir_effect_final_borrowed_read
    COMMAND test_xir_effect_final_read --new)
set_tests_properties(test_xir_effect_final_borrowed_read PROPERTIES
    LABELS "unit;xir;root-effects;ownership;metadata;final-read;producer-lifetime"
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1)
