# The original narrow and wide bitwise source retains its fixed result in every backend.
set(integer_width_source "${CMAKE_CURRENT_LIST_DIR}/test_source_product_integer_width.c")
set(integer_width_root "${product_consumer_fixture_root}/integer_width")
set(integer_width_c "${CMAKE_BINARY_DIR}/generated/source_product_integer_width.c")
add_executable(test_source_product_integer_width "${integer_width_source}")
target_link_libraries(test_source_product_integer_width PRIVATE source_product_consumer_driver)
add_custom_command(OUTPUT "${integer_width_c}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_source_product_integer_width> 0 "${integer_width_c}"
    DEPENDS test_source_product_integer_width "${integer_width_root}/root.xr" VERBATIM)
add_executable(test_source_product_integer_width_native "${integer_width_source}" "${integer_width_c}")
target_link_libraries(test_source_product_integer_width_native PRIVATE source_product_consumer_driver)
target_compile_definitions(test_source_product_integer_width_native PRIVATE XR_INTEGER_WIDTH_NATIVE=1)
foreach(target IN ITEMS test_source_product_integer_width test_source_product_integer_width_native)
    target_compile_definitions(${target} PRIVATE XR_INTEGER_WIDTH_ROOT="${integer_width_root}")
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_source_product_integer_width_vm COMMAND test_source_product_integer_width 0)
add_test(NAME test_source_product_integer_width_native COMMAND test_source_product_integer_width_native 1)
add_test(NAME test_source_product_integer_width_mixed_even COMMAND test_source_product_integer_width_native 2)
add_test(NAME test_source_product_integer_width_mixed_odd COMMAND test_source_product_integer_width_native 3)
set_tests_properties(test_source_product_integer_width_vm test_source_product_integer_width_native
    test_source_product_integer_width_mixed_even test_source_product_integer_width_mixed_odd
    PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;source-product;program-consumer;integer;ownership;execution")

# One explicit literal annotation is a control and never replaces the original source.
set(integer_width_control_root "${CMAKE_CURRENT_LIST_DIR}/integer_width_explicit_literal")
set(integer_width_control_c "${CMAKE_BINARY_DIR}/generated/source_product_integer_width_explicit_literal.c")
add_executable(test_source_product_integer_width_explicit_literal "${integer_width_source}")
target_link_libraries(test_source_product_integer_width_explicit_literal PRIVATE source_product_consumer_driver)
add_custom_command(OUTPUT "${integer_width_control_c}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_source_product_integer_width_explicit_literal> 0 "${integer_width_control_c}"
    DEPENDS test_source_product_integer_width_explicit_literal "${integer_width_control_root}/root.xr" VERBATIM)
add_executable(test_source_product_integer_width_explicit_literal_native "${integer_width_source}" "${integer_width_control_c}")
target_link_libraries(test_source_product_integer_width_explicit_literal_native PRIVATE source_product_consumer_driver)
target_compile_definitions(test_source_product_integer_width_explicit_literal_native PRIVATE XR_INTEGER_WIDTH_NATIVE=1)
foreach(target IN ITEMS test_source_product_integer_width_explicit_literal test_source_product_integer_width_explicit_literal_native)
    target_compile_definitions(${target} PRIVATE XR_INTEGER_WIDTH_ROOT="${integer_width_control_root}")
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_source_product_integer_width_explicit_literal_vm COMMAND test_source_product_integer_width_explicit_literal 0)
add_test(NAME test_source_product_integer_width_explicit_literal_native COMMAND test_source_product_integer_width_explicit_literal_native 1)
add_test(NAME test_source_product_integer_width_explicit_literal_mixed_even COMMAND test_source_product_integer_width_explicit_literal_native 2)
add_test(NAME test_source_product_integer_width_explicit_literal_mixed_odd COMMAND test_source_product_integer_width_explicit_literal_native 3)
set_tests_properties(test_source_product_integer_width_explicit_literal_vm test_source_product_integer_width_explicit_literal_native
    test_source_product_integer_width_explicit_literal_mixed_even test_source_product_integer_width_explicit_literal_mixed_odd
    PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;source-product;program-consumer;integer;ownership;control")
add_executable(test_source_product_integer_width_owned "${CMAKE_CURRENT_LIST_DIR}/test_source_product_integer_width_owned.c")
target_link_libraries(test_source_product_integer_width_owned PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_integer_width_owned PRIVATE "${CMAKE_CURRENT_LIST_DIR}/..")
set_target_properties(test_source_product_integer_width_owned PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_product_integer_width_owned PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_source_product_integer_width_owned PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_source_product_integer_width_original_owned COMMAND test_source_product_integer_width_owned
    "${integer_width_root}" "${integer_width_root}/root.xr" "${CMAKE_SOURCE_DIR}/stdlib")
add_test(NAME test_source_product_integer_width_explicit_literal_owned COMMAND test_source_product_integer_width_owned
    "${integer_width_control_root}" "${integer_width_control_root}/root.xr" "${CMAKE_SOURCE_DIR}/stdlib")
set_tests_properties(test_source_product_integer_width_original_owned test_source_product_integer_width_explicit_literal_owned
    PROPERTIES TIMEOUT 120 PROCESSORS 1 LABELS "unit;xir;source-product;program-consumer;integer;ownership;admission")
