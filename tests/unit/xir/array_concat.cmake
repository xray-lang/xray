# Genuine C is generated from the complete bounded Source pipeline.
set(CONCAT_C "${CMAKE_BINARY_DIR}/generated/xir_array_concat.c")
add_executable(test_xir_array_concat "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_array_concat.c")
target_link_libraries(test_xir_array_concat PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
add_custom_command(OUTPUT "${CONCAT_C}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_array_concat> 0 "${CONCAT_C}"
    DEPENDS test_xir_array_concat "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_array_concat/transaction/root.xr"
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_array_concat/transaction/producer.xr" VERBATIM)
add_executable(test_xir_array_concat_native "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_array_concat.c" "${CONCAT_C}")
target_link_libraries(test_xir_array_concat_native PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_xir_array_concat_native PRIVATE XR_CONCAT_NATIVE=1)
foreach(target test_xir_array_concat test_xir_array_concat_native)
    target_compile_definitions(${target} PRIVATE XR_CONCAT_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_array_concat")
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_xir_array_concat_vm COMMAND test_xir_array_concat 0)
add_test(NAME test_xir_array_concat_native COMMAND test_xir_array_concat_native 1)
add_test(NAME test_xir_array_concat_mixed_root COMMAND test_xir_array_concat_native 2)
add_test(NAME test_xir_array_concat_mixed_import COMMAND test_xir_array_concat_native 3)
set_tests_properties(test_xir_array_concat_vm test_xir_array_concat_native test_xir_array_concat_mixed_root
    test_xir_array_concat_mixed_import PROPERTIES TIMEOUT 120 LABELS "unit;xir;array;ownership;execution")
add_test(NAME test_xir_array_concat_source_rejections COMMAND test_xir_array_concat --rejections)
set_tests_properties(test_xir_array_concat_source_rejections PROPERTIES TIMEOUT 120 LABELS "unit;xir;array;ownership;permission")
