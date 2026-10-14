add_executable(test_xir_mixed_root_permissions xir/test_xir_mixed_root_permissions.c)
target_link_libraries(test_xir_mixed_root_permissions PRIVATE xray_xir_source)
target_compile_definitions(test_xir_mixed_root_permissions PRIVATE
    XR_ROOT_PERMISSION_FIXTURES="${CMAKE_SOURCE_DIR}/tests/unit/xir/fixtures/p20_root_permissions")
set_target_properties(test_xir_mixed_root_permissions PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_mixed_root_permissions PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_mixed_root_permissions PRIVATE -Wall -Wextra -Werror)
endif()
foreach(root_permission_case IN ITEMS pure-control local-worker-control snapshot-control generic-pure-control
    alias-pure-control cross-control mutual-control direct-read layered-read recursive-read generic-read
    alias-read cross-read mutual-read nested-snapshot-read direct-write)
    add_test(NAME test_xir_mixed_root_permission_${root_permission_case} COMMAND test_xir_mixed_root_permissions ${root_permission_case})
    set_tests_properties(test_xir_mixed_root_permission_${root_permission_case} PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE COST 1
        LABELS "unit;xir;mixed;source;root;permission;ownership;checked")
endforeach()
