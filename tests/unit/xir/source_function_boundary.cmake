if(TARGET test_xir_effect_analysis_context_controls)
    return()
endif()

add_executable(test_xir_effect_analysis_context_controls "${CMAKE_CURRENT_LIST_DIR}/test_xir_effect_analysis_context_controls.c")
target_link_libraries(test_xir_effect_analysis_context_controls PRIVATE xray_xir xray_xir_scalar xray_xir_source)
target_include_directories(test_xir_effect_analysis_context_controls PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/xir")
if(MSVC)
    target_compile_options(test_xir_effect_analysis_context_controls PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_effect_analysis_context_controls PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_effect_analysis_context_controls COMMAND test_xir_effect_analysis_context_controls)
set_tests_properties(test_xir_effect_analysis_context_controls PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1
    LABELS "unit;xir;root-effects;ownership;metadata")

find_package(Python3 REQUIRED COMPONENTS Interpreter)

include("${CMAKE_CURRENT_LIST_DIR}/effect_ordinary_environment.cmake")
add_test(NAME test_xir_source_enum_ref_places COMMAND test_xir_source_query --enum-ref-permission)
set_tests_properties(test_xir_source_enum_ref_places PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE
    LABELS "unit;xir;ownership;metadata;execution")
