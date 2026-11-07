# Complete Program fixtures remain source-free except the explicit Source guard.
set(XIR_NESTED_NULLABLE_C ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_nested_nullable.c)

add_executable(test_xir_nested_nullable_source xir/test_xir_nested_nullable_source.c)
target_link_libraries(test_xir_nested_nullable_source PRIVATE xray_xir_source_product)
target_compile_definitions(test_xir_nested_nullable_source PRIVATE
    XR_NESTED_SOURCE_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_nested_nullable_source")
add_executable(test_xir_nested_nullable_reader xir/test_xir_nested_nullable_reader.c)
target_link_libraries(test_xir_nested_nullable_reader PRIVATE xray_xir)
foreach(nested_target IN ITEMS test_xir_nested_nullable_source test_xir_nested_nullable_reader)
    if(MSVC)
        target_compile_options(${nested_target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${nested_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
foreach(nested_test IN ITEMS test_xir_nested_nullable_source)
    add_test(NAME ${nested_test} COMMAND ${nested_test})
    set_tests_properties(${nested_test} PROPERTIES LABELS "unit;xir;ownership;abi;execution" TIMEOUT 300)
endforeach()
add_test(NAME test_xir_nested_nullable_vectors COMMAND ${XRAY_PYTHON}
    ${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_nested_nullable_vectors.py)
set_tests_properties(test_xir_nested_nullable_vectors PROPERTIES LABELS "unit;xir;abi" TIMEOUT 30)

include(${CMAKE_CURRENT_SOURCE_DIR}/xir/source_nested_nullable.cmake)
