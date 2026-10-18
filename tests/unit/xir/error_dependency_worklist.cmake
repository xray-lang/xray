# Dependency-driven typed errors keep full checking and finite resource proofs.
add_executable(test_xir_error_dependency_worklist xir/test_xir_error_dependency_worklist.c)
target_link_libraries(test_xir_error_dependency_worklist PRIVATE xray_xir)
if(MSVC)
    target_compile_options(test_xir_error_dependency_worklist PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_error_dependency_worklist PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_error_dependency_worklist COMMAND test_xir_error_dependency_worklist)
set_tests_properties(test_xir_error_dependency_worklist PROPERTIES LABELS "unit;xir;metadata;ownership;budget" TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)
add_test(NAME test_xir_error_generic_restart_ownership COMMAND test_xir_error_dependency_worklist --restart-literals)
set_tests_properties(test_xir_error_generic_restart_ownership PROPERTIES
    LABELS "unit;xir;metadata;ownership;generic-effects" TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)
foreach(restart_gate IN ITEMS limits resources)
    add_test(NAME test_xir_error_generic_restart_${restart_gate}
        COMMAND test_xir_error_dependency_worklist --restart-${restart_gate})
    set_tests_properties(test_xir_error_generic_restart_${restart_gate} PROPERTIES
        LABELS "unit;xir;metadata;ownership;generic-effects;compiler-fault" TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)
endforeach()
