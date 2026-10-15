if(TEST test_xir_effect_source_function_capacity)
    return()
endif()
if(NOT TARGET test_xir_root_conditional_helpers)
    add_executable(test_xir_root_conditional_helpers "${CMAKE_CURRENT_LIST_DIR}/test_xir_root_conditional_helpers.c")
    target_link_libraries(test_xir_root_conditional_helpers PRIVATE xray_xir xray_xir_scalar xray_xir_source)
    target_include_directories(test_xir_root_conditional_helpers PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/tests/unit/xir")
    if(MSVC)
        target_compile_options(test_xir_root_conditional_helpers PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(test_xir_root_conditional_helpers PRIVATE -Wall -Wextra -Werror)
    endif()
endif()
add_test(NAME test_xir_effect_ordinary_environment_keys
    COMMAND test_xir_root_conditional_helpers --ordinary-environment-keys)
set_tests_properties(test_xir_effect_ordinary_environment_keys PROPERTIES
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1
    LABELS "unit;xir;ownership;metadata;compiler-fault;ordinary-context")

add_test(NAME test_xir_effect_source_function_capacity
    COMMAND test_xir_source_query --requirement-capacity)
add_test(NAME test_xir_effect_source_input_provenance
    COMMAND test_xir_source_query --requirement-provenance)
set_tests_properties(test_xir_effect_source_function_capacity test_xir_effect_source_input_provenance PROPERTIES
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1
    LABELS "unit;xir;ownership;metadata;ordinary-context;final-input")

add_test(NAME test_xir_effect_ordinary_environment_literals
    COMMAND test_xir_root_conditional_helpers --ordinary-environment-literals)
set_tests_properties(test_xir_effect_ordinary_environment_literals PROPERTIES
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1
    LABELS "unit;xir;ownership;metadata;ordinary-context;final-input")
