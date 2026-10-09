add_executable(test_original_witness_cleanup_source
    "${CMAKE_CURRENT_LIST_DIR}/test_original_witness_cleanup_source.c")
target_link_libraries(test_original_witness_cleanup_source PRIVATE xray_xir_source_product)
target_include_directories(test_original_witness_cleanup_source PRIVATE "${CMAKE_CURRENT_LIST_DIR}/..")
set_target_properties(test_original_witness_cleanup_source PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_original_witness_cleanup_source PRIVATE /utf-8 /W4 /WX)
    target_compile_definitions(test_original_witness_cleanup_source PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(test_original_witness_cleanup_source PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_original_witness_panic_only_defer_source COMMAND test_original_witness_cleanup_source
    witness_panic_only_defer "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/original_witness_panic_only_defer_root.xr" "${CMAKE_SOURCE_DIR}/stdlib")
add_test(NAME test_original_witness_invoke_defer_source COMMAND test_original_witness_cleanup_source
    witness_invoke_defer "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/original_witness_invoke_defer_root.xr" "${CMAKE_SOURCE_DIR}/stdlib")
set_tests_properties(test_original_witness_panic_only_defer_source test_original_witness_invoke_defer_source
    PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;source-product;program-consumer;witness;panic;defer;ownership;original-source")
