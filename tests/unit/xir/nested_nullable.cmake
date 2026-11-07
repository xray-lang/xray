# Complete Program fixtures remain source-free except the explicit Source guard.
set(XIR_NESTED_NULLABLE_C ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_nested_nullable.c)




add_executable(test_xir_nested_nullable_reader xir/test_xir_nested_nullable_reader.c)
target_link_libraries(test_xir_nested_nullable_reader PRIVATE xray_xir)
foreach(nested_target IN ITEMS  test_xir_nested_nullable_reader)
    if(MSVC)
        target_compile_options(${nested_target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${nested_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()

add_test(NAME test_xir_nested_nullable_vectors COMMAND ${XRAY_PYTHON}
    ${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_nested_nullable_vectors.py)
set_tests_properties(test_xir_nested_nullable_vectors PROPERTIES LABELS "unit;xir;abi" TIMEOUT 30)
