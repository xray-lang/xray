add_executable(test_xir_derivation_snapshot_match
    "${CMAKE_CURRENT_LIST_DIR}/test_xir_derivation_snapshot_match.c")
target_include_directories(test_xir_derivation_snapshot_match PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(test_xir_derivation_snapshot_match PRIVATE xray_xir xray_xir_scalar xray_xir_source)
set_target_properties(test_xir_derivation_snapshot_match PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_derivation_snapshot_match PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_xir_derivation_snapshot_match PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_derivation_snapshot_match COMMAND test_xir_derivation_snapshot_match)
set_tests_properties(test_xir_derivation_snapshot_match PROPERTIES
    TIMEOUT 60 RUN_SERIAL TRUE PROCESSORS 1 LABELS "unit;xir;compiler;ownership;metadata;derivation")
add_test(NAME test_xir_derivation_source_snapshot_match
    COMMAND test_xir_source_derivation --snapshot-facts)
set_tests_properties(test_xir_derivation_source_snapshot_match PROPERTIES
    TIMEOUT 60 RUN_SERIAL TRUE PROCESSORS 1 LABELS "unit;xir;source;compiler;ownership;metadata;derivation")
