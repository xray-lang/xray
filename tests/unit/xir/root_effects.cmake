add_executable(test_xir_root_effects ${CMAKE_CURRENT_LIST_DIR}/test_xir_root_effects.c)
target_include_directories(test_xir_root_effects PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(test_xir_root_effects PRIVATE xray_xir_source)
target_compile_definitions(test_xir_root_effects PRIVATE
    XR_ROOT_EFFECT_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_root_effects"
    XR_ROOT_EFFECT_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
if(MSVC)
    target_compile_options(test_xir_root_effects PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_root_effects PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_control_summary_owner COMMAND test_xir_root_effects --control-summary)
add_test(NAME test_xir_source_storage_owner COMMAND test_xir_root_effects --source-storage)
set_tests_properties(test_xir_control_summary_owner test_xir_source_storage_owner PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE PROCESSORS 1
    LABELS "unit;xir;metadata;ownership;compiler-fault")
