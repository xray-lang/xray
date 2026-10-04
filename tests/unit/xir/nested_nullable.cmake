# Complete Program fixtures remain source-free except the explicit Source guard.
set(XIR_NESTED_NULLABLE_C ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_nested_nullable.c)
add_executable(test_xir_nested_nullable xir/test_xir_nested_nullable.c)
target_link_libraries(test_xir_nested_nullable PRIVATE xray_xir_vm xray_xir_cgen)
add_custom_command(OUTPUT ${XIR_NESTED_NULLABLE_C}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND $<TARGET_FILE:test_xir_nested_nullable> ${XIR_NESTED_NULLABLE_C}
    DEPENDS test_xir_nested_nullable VERBATIM)
foreach(nested_mode IN ITEMS native mixed)
    add_executable(test_xir_nested_nullable_${nested_mode} xir/test_xir_nested_nullable_execution.c ${XIR_NESTED_NULLABLE_C})
    target_link_libraries(test_xir_nested_nullable_${nested_mode} PRIVATE xray_xir_vm)
    if(nested_mode STREQUAL "mixed")
        target_compile_definitions(test_xir_nested_nullable_${nested_mode} PRIVATE XR_NESTED_NULLABLE_MIXED=1)
    else()
        target_compile_definitions(test_xir_nested_nullable_${nested_mode} PRIVATE XR_NESTED_NULLABLE_MIXED=0)
    endif()
endforeach()
add_executable(test_xir_nested_nullable_source xir/test_xir_nested_nullable_source.c)
target_link_libraries(test_xir_nested_nullable_source PRIVATE xray_xir_source_product)
target_compile_definitions(test_xir_nested_nullable_source PRIVATE
    XR_NESTED_SOURCE_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_nested_nullable_source")
add_executable(test_xir_nested_nullable_reader xir/test_xir_nested_nullable_reader.c)
target_link_libraries(test_xir_nested_nullable_reader PRIVATE xray_xir)
foreach(nested_target IN ITEMS test_xir_nested_nullable test_xir_nested_nullable_native
    test_xir_nested_nullable_mixed test_xir_nested_nullable_source test_xir_nested_nullable_reader)
    if(MSVC)
        target_compile_options(${nested_target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${nested_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
foreach(nested_test IN ITEMS test_xir_nested_nullable test_xir_nested_nullable_native
    test_xir_nested_nullable_mixed test_xir_nested_nullable_source)
    add_test(NAME ${nested_test} COMMAND ${nested_test})
    set_tests_properties(${nested_test} PROPERTIES LABELS "unit;xir;ownership;abi;execution" TIMEOUT 300)
endforeach()
add_test(NAME test_xir_nested_nullable_vectors COMMAND ${XRAY_PYTHON}
    ${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_nested_nullable_vectors.py)
set_tests_properties(test_xir_nested_nullable_vectors PROPERTIES LABELS "unit;xir;abi" TIMEOUT 30)

include(${CMAKE_CURRENT_SOURCE_DIR}/xir/source_nested_nullable.cmake)
