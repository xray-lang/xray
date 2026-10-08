# The original three-module generic value struct source retains its fixed result in every backend.
set(generic_value_struct_source "${CMAKE_CURRENT_LIST_DIR}/test_source_product_generic_value_struct.c")
set(generic_value_struct_root "${product_consumer_fixture_root}/generic_value_struct")
set(generic_value_struct_c "${CMAKE_BINARY_DIR}/generated/source_product_generic_value_struct.c")
add_executable(test_source_product_generic_value_struct "${generic_value_struct_source}")
target_link_libraries(test_source_product_generic_value_struct PRIVATE source_product_consumer_driver)
add_custom_command(OUTPUT "${generic_value_struct_c}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_source_product_generic_value_struct> 0 "${generic_value_struct_c}"
    DEPENDS test_source_product_generic_value_struct "${generic_value_struct_root}/root.xr"
        "${generic_value_struct_root}/library.xr" "${generic_value_struct_root}/facade.xr" VERBATIM)
add_executable(test_source_product_generic_value_struct_native "${generic_value_struct_source}" "${generic_value_struct_c}")
target_link_libraries(test_source_product_generic_value_struct_native PRIVATE source_product_consumer_driver)
target_compile_definitions(test_source_product_generic_value_struct_native PRIVATE XR_GENERIC_VALUE_STRUCT_NATIVE=1)
foreach(target IN ITEMS test_source_product_generic_value_struct test_source_product_generic_value_struct_native)
    target_compile_definitions(${target} PRIVATE XR_GENERIC_VALUE_STRUCT_ROOT="${generic_value_struct_root}")
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_source_product_generic_value_struct_vm COMMAND test_source_product_generic_value_struct 0)
add_test(NAME test_source_product_generic_value_struct_native COMMAND test_source_product_generic_value_struct_native 1)
add_test(NAME test_source_product_generic_value_struct_mixed_even COMMAND test_source_product_generic_value_struct_native 2)
add_test(NAME test_source_product_generic_value_struct_mixed_odd COMMAND test_source_product_generic_value_struct_native 3)
set_tests_properties(test_source_product_generic_value_struct_vm test_source_product_generic_value_struct_native
    test_source_product_generic_value_struct_mixed_even test_source_product_generic_value_struct_mixed_odd
    PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;source-product;program-consumer;generic;ownership;execution")

add_executable(test_source_product_generic_value_struct_owned "${CMAKE_CURRENT_LIST_DIR}/test_source_product_generic_value_struct_owned.c")
target_link_libraries(test_source_product_generic_value_struct_owned PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_generic_value_struct_owned PRIVATE "${CMAKE_CURRENT_LIST_DIR}/..")
set_target_properties(test_source_product_generic_value_struct_owned PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_product_generic_value_struct_owned PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_source_product_generic_value_struct_owned PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_source_product_generic_value_struct_owned COMMAND test_source_product_generic_value_struct_owned
    "${generic_value_struct_root}" "${generic_value_struct_root}/root.xr" "${CMAKE_SOURCE_DIR}/stdlib")
set_tests_properties(test_source_product_generic_value_struct_owned PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;source-product;program-consumer;generic;ownership;admission")
