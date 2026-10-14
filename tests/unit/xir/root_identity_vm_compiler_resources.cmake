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
set(rb_expected_normal "454839;33661;3481828;1;280;123;157;401;122;300;122;122;122;122;122;122;122;330")
set(rb_expected_failure "454839;33661;3481201;1;280;123;157;401;122;300;122;122;122;122;122;122;122;330")
foreach(case owner_oom check_oom write_oom read_oom specialize_oom reverify_oom
    lower_oom bind_oom seal_oom budget samequota_retry)
    add_test(NAME test_xir_root_identity_vm_compiler_resources_${case}
        COMMAND test_xir_root_identity_vm_compiler_resources ${case}
            ${rb_expected_normal} ${rb_expected_failure})
    set_tests_properties(test_xir_root_identity_vm_compiler_resources_${case}
        PROPERTIES LABELS "unit;xir;root;task;ownership;compiler;resources"
        TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)
endforeach()
