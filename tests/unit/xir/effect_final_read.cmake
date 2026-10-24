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

add_test(NAME test_xir_effect_final_outcomes_literals
    COMMAND test_xir_error_cfg_pending --outcomes-literals)
set_tests_properties(test_xir_effect_final_outcomes_literals PROPERTIES
    LABELS "unit;xir;ownership;metadata;final-outcomes;callback;producer-lifetime"
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1)
add_test(NAME test_xir_effect_final_outcomes_resources
    COMMAND test_xir_error_cfg_pending --outcomes-resources)
set_tests_properties(test_xir_effect_final_outcomes_resources PROPERTIES
    LABELS "unit;xir;ownership;metadata;final-outcomes;callback;compiler-fault;resources"
    TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)

add_test(NAME test_xir_effect_source_ref_places
    COMMAND test_xir_source_query --enum-ref-next)
set_tests_properties(test_xir_effect_source_ref_places PROPERTIES
    LABELS "unit;xir;ownership;metadata;root-effects;source;producer-lifetime"
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1)

add_test(NAME test_xir_effect_default_dense
    COMMAND test_xir_default_invoke_source --generate-only
        ${CMAKE_CURRENT_BINARY_DIR}/effect-default-dense.xrc unused)
set_tests_properties(test_xir_effect_default_dense PROPERTIES
    LABELS "unit;xir;ownership;metadata;root-effects;source;default-arguments"
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1)

add_test(NAME test_xir_effect_final_root_trace
    COMMAND test_xir_root_cause_trace --literals)
set_tests_properties(test_xir_effect_final_root_trace PROPERTIES
    LABELS "unit;xir;ownership;metadata;root-effects;final-read;producer-lifetime"
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1)

add_test(NAME test_xir_effect_control_proof_literals COMMAND test_xir_error_cfg_pending --control-proof-literals)
add_test(NAME test_xir_effect_source_terminal_callback COMMAND test_xir_error_cfg_pending --source-terminal-callback)
set_tests_properties(test_xir_effect_control_proof_literals test_xir_effect_source_terminal_callback PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE LABELS "xir;metadata;ownership;final-input-proof")

add_executable(test_xir_effect_source_terminal_ordinary ${CMAKE_CURRENT_LIST_DIR}/test_xir_effect_source_terminal_ordinary.c)
target_include_directories(test_xir_effect_source_terminal_ordinary PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(test_xir_effect_source_terminal_ordinary PRIVATE xray_xir xray_xir_scalar xray_xir_source)
if(MSVC)
    target_compile_options(test_xir_effect_source_terminal_ordinary PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_effect_source_terminal_ordinary PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_effect_source_terminal_ordinary COMMAND test_xir_effect_source_terminal_ordinary)
set_tests_properties(test_xir_effect_source_terminal_ordinary PROPERTIES
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1 LABELS "unit;xir;metadata;ownership;final-input;producer-lifetime")
