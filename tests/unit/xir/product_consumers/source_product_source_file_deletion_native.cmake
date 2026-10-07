# Test-owned disk Source disappears before retained Checked/Lowered/native work.
set(source_delete_dir "${CMAKE_CURRENT_LIST_DIR}")
set(source_delete_generated "${CMAKE_BINARY_DIR}/generated/source-file-deletion-native1")
set(source_delete_root "${source_delete_generated}/private-source")
set(source_delete_c "${source_delete_generated}/program.c")
file(MAKE_DIRECTORY "${source_delete_root}")
add_executable(test_source_product_source_file_deletion_c_emitter
    "${source_delete_dir}/test_source_product_source_file_deletion_native.c")
target_link_libraries(test_source_product_source_file_deletion_c_emitter PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_source_file_deletion_c_emitter PRIVATE "${source_delete_dir}/..")
add_custom_command(OUTPUT "${source_delete_c}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${source_delete_root}"
    COMMAND $<TARGET_FILE:test_source_product_source_file_deletion_c_emitter>
        "${source_delete_root}" "${source_delete_root}/main.xr" "${source_delete_root}/library.xr"
        --emit "${source_delete_c}"
    DEPENDS test_source_product_source_file_deletion_c_emitter
        "${source_delete_dir}/test_source_product_source_file_deletion_native.c"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    VERBATIM)
add_executable(test_source_product_source_file_deletion_native
    "${source_delete_dir}/test_source_product_source_file_deletion_native.c" "${source_delete_c}")
target_compile_definitions(test_source_product_source_file_deletion_native PRIVATE XR_SOURCE_FILE_DELETION_NATIVE=1)
target_link_libraries(test_source_product_source_file_deletion_native PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_source_file_deletion_native PRIVATE "${source_delete_dir}/..")
foreach(target IN ITEMS test_source_product_source_file_deletion_c_emitter test_source_product_source_file_deletion_native)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_source_product_source_file_deletion_native_normal COMMAND test_source_product_source_file_deletion_native
    "${source_delete_root}" "${source_delete_root}/main.xr" "${source_delete_root}/library.xr")
set_tests_properties(test_source_product_source_file_deletion_native_normal PROPERTIES
    TIMEOUT 120 PROCESSORS 1 WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    LABELS "unit;xir;source-product;program-consumer;typed-error;ownership;native-projection;physical-source-deletion")
