# Root test roles and Atomic ordering use the same public source product.
set(entry_roles_dir "${CMAKE_CURRENT_LIST_DIR}")
set(entry_roles_cases
    original_canonical_initializer original_test_discovery
    original_atomic_ordering_Relaxed original_atomic_ordering_Acquire
    original_atomic_ordering_Release original_atomic_ordering_AcquireRelease original_atomic_ordering_SeqCst
    legal_rmw_Relaxed legal_rmw_Acquire legal_rmw_Release legal_rmw_AcquireRelease legal_rmw_SeqCst
    legal_test_discovery legal_original_Relaxed legal_original_SeqCst)
add_executable(test_source_product_entry_roles_probe "${entry_roles_dir}/source_product_entry_roles_probe.c")
target_link_libraries(test_source_product_entry_roles_probe PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_entry_roles_probe PRIVATE "${entry_roles_dir}/..")
set_target_properties(test_source_product_entry_roles_probe PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_product_entry_roles_probe PRIVATE /utf-8 /W4 /WX)
    target_compile_definitions(test_source_product_entry_roles_probe PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(test_source_product_entry_roles_probe PRIVATE -Wall -Wextra -Werror)
endif()
foreach(case IN LISTS entry_roles_cases)
    set(test "test_source_product_entry_roles_${case}")
    add_test(NAME ${test}
        COMMAND ${XRAY_PYTHON} -X utf8 "${entry_roles_dir}/source_product_entry_roles_oracle.py"
            --binary $<TARGET_FILE:test_source_product_entry_roles_probe> --case ${case}
            --fixtures "${entry_roles_dir}/entry_roles/fixtures"
            --scratch "${CMAKE_BINARY_DIR}/consumer-entry-role-evidence")
    set_tests_properties(${test} PROPERTIES TIMEOUT 120
        LABELS "unit;xir;source-product;program-consumer;ownership;entry-roles;vm-projection")
endforeach()
