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
