# Complete original imported enum Source; independent VM after physical leaf deletion.
set(source_file_deletion_vm_dir "${CMAKE_CURRENT_LIST_DIR}")
set(source_file_deletion_vm_root "${CMAKE_BINARY_DIR}/generated/source-file-deletion-vm1/private-source")
file(MAKE_DIRECTORY "${source_file_deletion_vm_root}")
add_executable(test_source_product_source_file_deletion_vm
    "${source_file_deletion_vm_dir}/test_source_product_source_file_deletion_vm.c")
target_link_libraries(test_source_product_source_file_deletion_vm PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_source_file_deletion_vm PRIVATE "${source_file_deletion_vm_dir}/..")
set_target_properties(test_source_product_source_file_deletion_vm PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_product_source_file_deletion_vm PRIVATE /utf-8 /W4 /WX)
    target_compile_definitions(test_source_product_source_file_deletion_vm PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(test_source_product_source_file_deletion_vm PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_source_product_source_file_deletion_vm_normal COMMAND test_source_product_source_file_deletion_vm
    "${source_file_deletion_vm_root}" "${source_file_deletion_vm_root}/main.xr" "${source_file_deletion_vm_root}/library.xr")
set_tests_properties(test_source_product_source_file_deletion_vm_normal PROPERTIES TIMEOUT 120 PROCESSORS 1
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    LABELS "unit;xir;source-product;program-consumer;typed-error;ownership;vm-projection;source-deletion")
