add_executable(test_xir_instance_bindings xir/test_xir_instance_bindings.c)
target_link_libraries(test_xir_instance_bindings PRIVATE xray_xir_vm xray_xir_cgen)
if(MSVC)
    target_compile_options(test_xir_instance_bindings PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_xir_instance_bindings PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_instance_bindings COMMAND test_xir_instance_bindings)
set_tests_properties(test_xir_instance_bindings PROPERTIES
    LABELS "unit;xir;lifecycle;ownership;generation;binding" TIMEOUT 120)
add_test(NAME test_xir_instance_bindings_prepare_resources
    COMMAND test_xir_instance_bindings --prepare-resources)
set_tests_properties(test_xir_instance_bindings_prepare_resources PROPERTIES
    LABELS "unit;xir;lifecycle;ownership;generation;binding;fault-injection;resources"
    RUN_SERIAL TRUE TIMEOUT 120 COST 1)
