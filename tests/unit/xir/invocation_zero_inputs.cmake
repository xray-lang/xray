add_executable(test_xir_invocation_zero_inputs ${CMAKE_CURRENT_LIST_DIR}/test_xir_invocation_zero_inputs.c)
target_include_directories(test_xir_invocation_zero_inputs PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(test_xir_invocation_zero_inputs PRIVATE xray_xir xray_xir_scalar xray_xir_source)
if(MSVC)
    target_compile_options(test_xir_invocation_zero_inputs PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_invocation_zero_inputs PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_invocation_zero_inputs COMMAND test_xir_invocation_zero_inputs)
set_tests_properties(test_xir_invocation_zero_inputs PROPERTIES
    LABELS "unit;xir;root-effects;compiler;ownership" TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)
add_test(NAME test_xir_invocation_zero_inputs_compiler COMMAND test_xir_invocation_zero_inputs --compiler)
set_tests_properties(test_xir_invocation_zero_inputs_compiler PROPERTIES
    LABELS "unit;xir;root-effects;compiler-fault;ownership" TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)
