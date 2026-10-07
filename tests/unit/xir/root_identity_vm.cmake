add_executable(test_xir_root_identity_vm xir/test_xir_root_identity_vm.c)
target_link_libraries(test_xir_root_identity_vm PRIVATE xray_xir_vm xray_xir_scalar)
if(MSVC)
    target_compile_options(test_xir_root_identity_vm PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_root_identity_vm PRIVATE -Wall -Wextra -Werror)
endif()
foreach(case identity resume closing initialization_failure)
    add_test(NAME test_xir_root_identity_vm_${case}
        COMMAND test_xir_root_identity_vm ${case})
    set_tests_properties(test_xir_root_identity_vm_${case}
        PROPERTIES LABELS "unit;xir;root;task;ownership;runtime" TIMEOUT 60)
endforeach()
