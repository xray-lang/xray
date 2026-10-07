add_executable(test_xir_root_identity_vm_compiler_resources
    xir/test_xir_root_identity_vm_compiler_resources.c)
target_link_libraries(test_xir_root_identity_vm_compiler_resources PRIVATE xray_xir_vm xray_xir_scalar)
if(MSVC)
    target_compile_options(test_xir_root_identity_vm_compiler_resources PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_root_identity_vm_compiler_resources PRIVATE -Wall -Wextra -Werror)
endif()
foreach(case normal census)
    add_test(NAME test_xir_root_identity_vm_compiler_resources_${case}
        COMMAND test_xir_root_identity_vm_compiler_resources ${case})
    set_tests_properties(test_xir_root_identity_vm_compiler_resources_${case}
        PROPERTIES LABELS "unit;xir;root;task;ownership;compiler;resources"
        TIMEOUT 60 RUN_SERIAL TRUE PROCESSORS 1)
endforeach()

# Independently measured complete compiler frontiers for both real VM graphs.
set(rb_expected_normal "97495;20577;261654;1;86;27;60;112;26;135;29;29;29;29;29;29;29;177")
set(rb_expected_failure "97495;20577;261654;1;86;27;60;112;26;135;29;29;29;29;29;29;29;177")
foreach(case owner_oom check_oom write_oom read_oom specialize_oom reverify_oom
    lower_oom bind_oom seal_oom budget samequota_retry)
    add_test(NAME test_xir_root_identity_vm_compiler_resources_${case}
        COMMAND test_xir_root_identity_vm_compiler_resources ${case}
            ${rb_expected_normal} ${rb_expected_failure})
    set_tests_properties(test_xir_root_identity_vm_compiler_resources_${case}
        PROPERTIES LABELS "unit;xir;root;task;ownership;compiler;resources"
        TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)
endforeach()
