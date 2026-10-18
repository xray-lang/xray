add_executable(test_lsp_index_pool "${CMAKE_CURRENT_LIST_DIR}/test_index_pool.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_source_index.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_index_scan.c")
target_link_libraries(test_lsp_index_pool PRIVATE xray_lsp_source_syntax)
target_include_directories(test_lsp_index_pool PRIVATE "${PROJECT_SOURCE_DIR}/src/base")
target_compile_features(test_lsp_index_pool PRIVATE c_std_11)
if(MSVC)
    target_compile_options(test_lsp_index_pool PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_lsp_index_pool PRIVATE -Wall -Wextra -Werror)
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME test_lsp_index_pool COMMAND "${Python3_EXECUTABLE}" -X utf8
    "${CMAKE_CURRENT_LIST_DIR}/test_index_pool.py" "$<TARGET_FILE:test_lsp_index_pool>")
set_tests_properties(test_lsp_index_pool PROPERTIES TIMEOUT 30 LABELS "unit;lsp;ownership;metadata;allocation")
