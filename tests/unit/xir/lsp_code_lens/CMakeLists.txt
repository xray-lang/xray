if(TARGET test_lsp_code_lens)
    return()
endif()
add_executable(test_lsp_code_lens "${CMAKE_CURRENT_LIST_DIR}/test_code_lens.c")
target_link_libraries(test_lsp_code_lens PRIVATE xray_lsp_source_workspace)
target_include_directories(test_lsp_code_lens PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/xir")
target_compile_features(test_lsp_code_lens PRIVATE c_std_11)
if(WIN32)
    target_link_libraries(test_lsp_code_lens PRIVATE bcrypt)
endif()
if(MSVC)
    target_compile_options(test_lsp_code_lens PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_lsp_code_lens PRIVATE -Wall -Wextra -Werror)
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME test_lsp_code_lens COMMAND "${Python3_EXECUTABLE}" -X utf8
    "${CMAKE_CURRENT_LIST_DIR}/test_code_lens.py" "$<TARGET_FILE:test_lsp_code_lens>"
    "${PROJECT_SOURCE_DIR}/stdlib")
set_tests_properties(test_lsp_code_lens PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE
    LABELS "unit;lsp;ownership;metadata;allocation")
add_test(NAME test_lsp_code_lens_normal COMMAND "${Python3_EXECUTABLE}" -X utf8
    "${CMAKE_CURRENT_LIST_DIR}/test_code_lens.py" "$<TARGET_FILE:test_lsp_code_lens>"
    "${PROJECT_SOURCE_DIR}/stdlib" --normal)
set_tests_properties(test_lsp_code_lens_normal PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE
    LABELS "unit;lsp;ownership;metadata")
