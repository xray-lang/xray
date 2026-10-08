# Source preserves the managed String append fixture's fixed length in every backend.
set(array_append_managed "${CMAKE_CURRENT_LIST_DIR}/test_source_product_array_append_managed.c")
set(array_append_root "${CMAKE_CURRENT_LIST_DIR}/array_append_managed")
set(array_append_c "${CMAKE_BINARY_DIR}/generated/source_product_array_append_managed.c")
add_executable(test_source_product_array_append_managed "${array_append_managed}")
target_link_libraries(test_source_product_array_append_managed PRIVATE source_product_consumer_driver)
add_custom_command(OUTPUT "${array_append_c}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_source_product_array_append_managed> 0 "${array_append_c}"
    DEPENDS test_source_product_array_append_managed "${array_append_root}/root.xr" VERBATIM)
add_executable(test_source_product_array_append_managed_native "${array_append_managed}" "${array_append_c}")
target_link_libraries(test_source_product_array_append_managed_native PRIVATE source_product_consumer_driver)
target_compile_definitions(test_source_product_array_append_managed_native PRIVATE XR_ARRAY_MANAGED_NATIVE=1)
add_executable(test_source_product_array_append_managed_owned "${CMAKE_CURRENT_LIST_DIR}/test_source_product_array_append_managed_owned.c")
target_link_libraries(test_source_product_array_append_managed_owned PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_array_append_managed_owned PRIVATE "${CMAKE_CURRENT_LIST_DIR}/..")
foreach(target IN ITEMS test_source_product_array_append_managed test_source_product_array_append_managed_native test_source_product_array_append_managed_owned)
    target_compile_definitions(${target} PRIVATE XR_ARRAY_MANAGED_ROOT="${array_append_root}")
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_source_product_array_append_managed_vm COMMAND test_source_product_array_append_managed 0)
add_test(NAME test_source_product_array_append_managed_native COMMAND test_source_product_array_append_managed_native 1)
add_test(NAME test_source_product_array_append_managed_mixed_even COMMAND test_source_product_array_append_managed_native 2)
add_test(NAME test_source_product_array_append_managed_mixed_odd COMMAND test_source_product_array_append_managed_native 3)
add_test(NAME test_source_product_array_append_managed_owned COMMAND test_source_product_array_append_managed_owned
    "${array_append_root}" "${array_append_root}/root.xr" "${CMAKE_SOURCE_DIR}/stdlib")
set_tests_properties(test_source_product_array_append_managed_vm test_source_product_array_append_managed_native
    test_source_product_array_append_managed_mixed_even test_source_product_array_append_managed_mixed_odd
    test_source_product_array_append_managed_owned PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;source-product;program-consumer;array;ownership;execution")
foreach(mode RANGE 0 3)
    if(mode EQUAL 0)
        set(binary test_source_product_array_append_managed)
    else()
        set(binary test_source_product_array_append_managed_native)
    endif()
    add_test(NAME test_source_product_array_append_managed_axes_${mode} COMMAND ${binary} ${mode} --compiler-axes)
    add_test(NAME test_source_product_array_append_managed_runtime_${mode} COMMAND ${binary} ${mode} --runtime-allocations)
    add_test(NAME test_source_product_array_append_managed_cancel_${mode} COMMAND ${binary} ${mode} --cancel)
    set_tests_properties(test_source_product_array_append_managed_axes_${mode} test_source_product_array_append_managed_runtime_${mode}
        test_source_product_array_append_managed_cancel_${mode} PROPERTIES TIMEOUT 120 PROCESSORS 1
        LABELS "unit;xir;source-product;program-consumer;array;ownership;allocations")
    set_tests_properties(test_source_product_array_append_managed_axes_${mode} PROPERTIES RUN_SERIAL TRUE)
    add_test(NAME test_source_product_array_append_managed_compiler_${mode}
        COMMAND ${XRAY_PYTHON} -B -X utf8 "${CMAKE_SOURCE_DIR}/scripts/source_product_consumer_allocation_faults.py" run
            --binary $<TARGET_FILE:${binary}> --mode ${mode} --jobs 8 --timeout 600
            --root "${array_append_root}" --file "${array_append_root}/root.xr" --input-root "${CMAKE_SOURCE_DIR}"
            --source-file "${array_append_managed}" --registration-file "${CMAKE_CURRENT_LIST_FILE}"
            --evidence "${CMAKE_BINARY_DIR}/consumer-fi/array_append_managed-${mode}")
    set_tests_properties(test_source_product_array_append_managed_compiler_${mode} PROPERTIES TIMEOUT 600 PROCESSORS 8 COST 300
        LABELS "unit;xir;source-product;program-consumer;array;ownership;allocations;compiler-fi")
endforeach()
