add_executable(test_xir_root_identity_vm_resources xir/test_xir_root_identity_vm_resources.c)
target_link_libraries(test_xir_root_identity_vm_resources PRIVATE xray_xir_vm xray_xir_scalar)
if(MSVC)
    target_compile_options(test_xir_root_identity_vm_resources PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_root_identity_vm_resources PRIVATE -Wall -Wextra -Werror)
endif()
foreach(case normal create_oom prepare_oom prepare_budget)
    add_test(NAME test_xir_root_identity_vm_resources_${case}
        COMMAND test_xir_root_identity_vm_resources ${case})
    set_tests_properties(test_xir_root_identity_vm_resources_${case}
        PROPERTIES LABELS "unit;xir;root;task;ownership;runtime;resources" TIMEOUT 60)
endforeach()
