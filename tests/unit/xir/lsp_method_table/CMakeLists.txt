if(TARGET test_lsp_method_table)
    return()
endif()
add_executable(test_lsp_method_table "${CMAKE_CURRENT_LIST_DIR}/test_table.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_lifecycle_state.c"
    "${PROJECT_SOURCE_DIR}/src/base/xjson.c" "${PROJECT_SOURCE_DIR}/src/base/xutf8.c")
target_include_directories(test_lsp_method_table PRIVATE "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/src/base" "${PROJECT_SOURCE_DIR}/include")
target_compile_features(test_lsp_method_table PRIVATE c_std_11)
if(MSVC)
    target_compile_options(test_lsp_method_table PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_lsp_method_table PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_lsp_method_table COMMAND test_lsp_method_table)
set_tests_properties(test_lsp_method_table PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE LABELS "unit;lsp;ownership;allocation")
