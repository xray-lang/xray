# The test overrides the real compiler-resource allocator to observe physical owners.
add_executable(test_xir_ctfe_leaf "${CMAKE_SOURCE_DIR}/tests/unit/xir/test_xir_ctfe_leaf.c")
target_link_libraries(test_xir_ctfe_leaf PRIVATE xray_xir_source xray_xir_vm)
set_target_properties(test_xir_ctfe_leaf PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_ctfe_leaf PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_xir_ctfe_leaf PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_ctfe_leaf COMMAND test_xir_ctfe_leaf)
set_tests_properties(test_xir_ctfe_leaf PROPERTIES LABELS "unit;xir;metadata;ownership;ctfe" TIMEOUT 60)

add_test(NAME test_xir_ctfe_calls COMMAND test_xir_ctfe_leaf --calls)
set_tests_properties(test_xir_ctfe_calls PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE PROCESSORS 1
    LABELS "unit;xir;metadata;ownership;ctfe;direct-call")
add_test(NAME test_xir_ctfe_calls_resources COMMAND test_xir_ctfe_leaf --call-resources)
set_tests_properties(test_xir_ctfe_calls_resources PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE PROCESSORS 1
    LABELS "unit;xir;metadata;ownership;compiler-fault;ctfe;direct-call")
