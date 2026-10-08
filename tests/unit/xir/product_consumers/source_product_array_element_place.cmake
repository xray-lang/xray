# The original six element place cases execute through each independently fixed backend.
set(array_element_place "${CMAKE_CURRENT_LIST_DIR}/test_source_product_array_element_place.c")
set(array_element_place_root "${CMAKE_CURRENT_LIST_DIR}/array_element_place")
set(array_element_place_c "${CMAKE_BINARY_DIR}/generated/source_product_array_element_place.c")
add_executable(test_source_product_array_element_place "${array_element_place}")
target_link_libraries(test_source_product_array_element_place PRIVATE source_product_consumer_driver)
add_custom_command(OUTPUT "${array_element_place_c}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_source_product_array_element_place> 0 "${array_element_place_c}"
    DEPENDS test_source_product_array_element_place "${array_element_place_root}/root.xr" VERBATIM)
add_executable(test_source_product_array_element_place_native "${array_element_place}" "${array_element_place_c}")
target_link_libraries(test_source_product_array_element_place_native PRIVATE source_product_consumer_driver)
target_compile_definitions(test_source_product_array_element_place_native PRIVATE XR_ARRAY_ELEMENT_PLACE_NATIVE=1)
add_executable(test_source_product_array_element_place_owned "${CMAKE_CURRENT_LIST_DIR}/test_source_product_array_element_place_owned.c")
target_link_libraries(test_source_product_array_element_place_owned PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_array_element_place_owned PRIVATE "${CMAKE_CURRENT_LIST_DIR}/..")
foreach(target IN ITEMS test_source_product_array_element_place test_source_product_array_element_place_native test_source_product_array_element_place_owned)
    target_compile_definitions(${target} PRIVATE XR_ARRAY_ELEMENT_PLACE_ROOT="${array_element_place_root}")
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_source_product_array_element_place_vm COMMAND test_source_product_array_element_place 0)
add_test(NAME test_source_product_array_element_place_native COMMAND test_source_product_array_element_place_native 1)
add_test(NAME test_source_product_array_element_place_mixed_even COMMAND test_source_product_array_element_place_native 2)
add_test(NAME test_source_product_array_element_place_mixed_odd COMMAND test_source_product_array_element_place_native 3)
add_test(NAME test_source_product_array_element_place_owned COMMAND test_source_product_array_element_place_owned
    "${array_element_place_root}" "${array_element_place_root}/root.xr" "${CMAKE_SOURCE_DIR}/stdlib")
set_tests_properties(test_source_product_array_element_place_vm test_source_product_array_element_place_native
    test_source_product_array_element_place_mixed_even test_source_product_array_element_place_mixed_odd
    test_source_product_array_element_place_owned PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;source-product;program-consumer;array;ownership;execution")
foreach(mode RANGE 0 3)
    if(mode EQUAL 0)
        set(binary test_source_product_array_element_place)
    else()
        set(binary test_source_product_array_element_place_native)
    endif()
    add_test(NAME test_source_product_array_element_place_axes_${mode} COMMAND ${binary} ${mode} --axes)
    add_test(NAME test_source_product_array_element_place_runtime_${mode} COMMAND ${binary} ${mode} --runtime)
    add_test(NAME test_source_product_array_element_place_cancel_${mode} COMMAND ${binary} ${mode} --cancel)
    set_tests_properties(test_source_product_array_element_place_axes_${mode} test_source_product_array_element_place_runtime_${mode}
        test_source_product_array_element_place_cancel_${mode} PROPERTIES TIMEOUT 120 PROCESSORS 1
        LABELS "unit;xir;source-product;program-consumer;array;ownership;allocations")
    set_tests_properties(test_source_product_array_element_place_axes_${mode} PROPERTIES RUN_SERIAL TRUE)
    add_test(NAME test_source_product_array_element_place_compiler_${mode}
        COMMAND ${XRAY_PYTHON} -B -X utf8 "${product_consumer_fault_runner}"
            --binary $<TARGET_FILE:${binary}> --mode ${mode} --jobs 8
            --evidence "${CMAKE_BINARY_DIR}/consumer-fi/array_element_place-${mode}")
    set_tests_properties(test_source_product_array_element_place_compiler_${mode} PROPERTIES TIMEOUT 600 PROCESSORS 8 COST 300 RUN_SERIAL TRUE
        LABELS "unit;xir;source-product;program-consumer;array;ownership;allocations;compiler-fi")
endforeach()
