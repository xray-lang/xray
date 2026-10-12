# Actual local CFG dependencies retain all compiler ownership and budget gates.
add_executable(test_xir_error_cfg_pending xir/test_xir_error_cfg_pending.c)
target_link_libraries(test_xir_error_cfg_pending PRIVATE xray_xir)
if(MSVC)
    target_compile_options(test_xir_error_cfg_pending PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_error_cfg_pending PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_error_cfg_pending COMMAND test_xir_error_cfg_pending)
set_tests_properties(test_xir_error_cfg_pending PROPERTIES
    LABELS "unit;xir;metadata;ownership;budget" TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)


# Independent feedback does not replace the original aggregate deadline.
foreach(pending_kind IN ITEMS semantics performance resources)
    add_test(NAME test_xir_error_cfg_pending_${pending_kind}
        COMMAND test_xir_error_cfg_pending --${pending_kind})
    set_tests_properties(test_xir_error_cfg_pending_${pending_kind} PROPERTIES
        LABELS "unit;xir;metadata;ownership;pending-${pending_kind};partial-feedback"
        TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)
endforeach()
