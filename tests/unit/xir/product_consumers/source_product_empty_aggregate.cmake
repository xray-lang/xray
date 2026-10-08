# Empty aggregate construction keeps the original scalar42 in every independent backend.
set(empty_aggregate "${CMAKE_CURRENT_LIST_DIR}/test_source_product_empty_aggregate.c")
set(empty_aggregate_root "${CMAKE_CURRENT_LIST_DIR}/empty_aggregate")
set(empty_aggregate_c "${CMAKE_BINARY_DIR}/generated/source_product_empty_aggregate.c")
add_executable(test_source_product_empty_aggregate "${empty_aggregate}")
target_link_libraries(test_source_product_empty_aggregate PRIVATE source_product_consumer_driver)
add_custom_command(OUTPUT "${empty_aggregate_c}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_source_product_empty_aggregate> 0 "${empty_aggregate_c}"
    DEPENDS test_source_product_empty_aggregate "${empty_aggregate_root}/root.xr" VERBATIM)
add_executable(test_source_product_empty_aggregate_native "${empty_aggregate}" "${empty_aggregate_c}")
target_link_libraries(test_source_product_empty_aggregate_native PRIVATE source_product_consumer_driver)
target_compile_definitions(test_source_product_empty_aggregate_native PRIVATE XR_EMPTY_AGGREGATE_NATIVE=1)
add_executable(test_source_product_empty_aggregate_owned "${CMAKE_CURRENT_LIST_DIR}/test_source_product_empty_aggregate_owned.c")
target_link_libraries(test_source_product_empty_aggregate_owned PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_empty_aggregate_owned PRIVATE "${CMAKE_CURRENT_LIST_DIR}/..")
foreach(target IN ITEMS test_source_product_empty_aggregate test_source_product_empty_aggregate_native test_source_product_empty_aggregate_owned)
    target_compile_definitions(${target} PRIVATE XR_EMPTY_AGGREGATE_ROOT="${empty_aggregate_root}")
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_source_product_empty_aggregate_vm COMMAND test_source_product_empty_aggregate 0)
add_test(NAME test_source_product_empty_aggregate_native COMMAND test_source_product_empty_aggregate_native 1)
add_test(NAME test_source_product_empty_aggregate_mixed_even COMMAND test_source_product_empty_aggregate_native 2)
add_test(NAME test_source_product_empty_aggregate_mixed_odd COMMAND test_source_product_empty_aggregate_native 3)
add_test(NAME test_source_product_empty_aggregate_owned COMMAND test_source_product_empty_aggregate_owned
    "${empty_aggregate_root}" "${empty_aggregate_root}/root.xr" "${CMAKE_SOURCE_DIR}/stdlib")
set_tests_properties(test_source_product_empty_aggregate_vm test_source_product_empty_aggregate_native
    test_source_product_empty_aggregate_mixed_even test_source_product_empty_aggregate_mixed_odd
    test_source_product_empty_aggregate_owned PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;source-product;program-consumer;array;ownership;execution")
foreach(mode RANGE 0 3)
    if(mode EQUAL 0)
        set(binary test_source_product_empty_aggregate)
    else()
        set(binary test_source_product_empty_aggregate_native)
    endif()
    add_test(NAME test_source_product_empty_aggregate_axes_${mode} COMMAND ${binary} ${mode} --axes)
    add_test(NAME test_source_product_empty_aggregate_runtime_${mode} COMMAND ${binary} ${mode} --runtime)
    add_test(NAME test_source_product_empty_aggregate_cancel_${mode} COMMAND ${binary} ${mode} --cancel)
    set_tests_properties(test_source_product_empty_aggregate_axes_${mode} test_source_product_empty_aggregate_runtime_${mode}
        test_source_product_empty_aggregate_cancel_${mode} PROPERTIES TIMEOUT 120 PROCESSORS 1
        LABELS "unit;xir;source-product;program-consumer;array;ownership;allocations")
    set_tests_properties(test_source_product_empty_aggregate_axes_${mode} PROPERTIES RUN_SERIAL TRUE)
    add_test(NAME test_source_product_empty_aggregate_compiler_${mode}
        COMMAND ${XRAY_PYTHON} -B -X utf8 "${product_consumer_fault_runner}"
            --binary $<TARGET_FILE:${binary}> --mode ${mode} --jobs 8
            --evidence "${CMAKE_BINARY_DIR}/consumer-fi/empty_aggregate-${mode}")
    set_tests_properties(test_source_product_empty_aggregate_compiler_${mode} PROPERTIES TIMEOUT 600 PROCESSORS 8 COST 300 RUN_SERIAL TRUE
        LABELS "unit;xir;source-product;program-consumer;array;ownership;allocations;compiler-fi")
endforeach()
