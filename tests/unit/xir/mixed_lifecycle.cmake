# Keep the full cleanup/FI registration and add independent late-event oracles.
add_executable(test_xir_effects_flow_storage xir/test_xir_effects_flow_storage.c)
target_link_libraries(test_xir_effects_flow_storage PRIVATE xray_xir_vm)
set_target_properties(test_xir_effects_flow_storage PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_effects_flow_storage PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_effects_flow_storage PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_effects_flow_storage COMMAND test_xir_effects_flow_storage)
set_tests_properties(test_xir_effects_flow_storage PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE COST 1 LABELS "unit;xir;effects;ownership;resource")
add_test(NAME test_xir_effects_flow_storage_fi COMMAND test_xir_effects_flow_storage --fi)
set_tests_properties(test_xir_effects_flow_storage_fi PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE COST 1 LABELS "unit;xir;effects;ownership;resource;fault-injection")
foreach(native_cleanup IN ITEMS 0 1)
    add_test(NAME test_xir_mixed_lifecycle_${native_cleanup}
        COMMAND test_xir_cleanup_mixed --lifecycle ${native_cleanup})
    set_tests_properties(test_xir_mixed_lifecycle_${native_cleanup} PROPERTIES
        TIMEOUT 120 RUN_SERIAL TRUE COST 1
        LABELS "unit;xir;mixed;execution;ownership;instance;abi;lifecycle")
endforeach()
