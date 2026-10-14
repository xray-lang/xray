add_executable(test_lsp_source_index_store "${CMAKE_CURRENT_LIST_DIR}/test_index_store.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_source_index.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_source_index_store.c")
target_link_libraries(test_lsp_source_index_store PRIVATE xray_lsp_source_syntax)
target_compile_features(test_lsp_source_index_store PRIVATE c_std_11)
if(MSVC)
    target_compile_options(test_lsp_source_index_store PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_lsp_source_index_store PRIVATE -Wall -Wextra -Werror)
endif()
enable_testing()
add_test(NAME test_lsp_source_index_store COMMAND test_lsp_source_index_store)
set_tests_properties(test_lsp_source_index_store PROPERTIES TIMEOUT 30 LABELS "unit;lsp;ownership;metadata;allocation")
