add_executable(test_xir_class_stage_contract xir/test_xir_class_stage_contract.c)
target_link_libraries(test_xir_class_stage_contract PRIVATE xray_xir)
target_compile_definitions(test_xir_class_stage_contract PRIVATE
    XR_CLASS_STAGE_PREVIOUS="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_records/previous-common129-staged-ctor"
    XR_CLASS_STAGE_PREVIOUS130="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_records/previous-common130-staged-ctor")
if(MSVC)
    target_compile_options(test_xir_class_stage_contract PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_class_stage_contract PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_class_stage_contract COMMAND test_xir_class_stage_contract)
add_test(NAME test_xir_class_stage_raw_contract COMMAND test_xir_class_stage_contract --raw-only)
set_tests_properties(test_xir_class_stage_contract test_xir_class_stage_raw_contract PROPERTIES
    LABELS "unit;xir;ownership;nominal" TIMEOUT 180)
