if(TARGET test_lsp_original_config_helpers)
    return()
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(original_config_source "${PROJECT_SOURCE_DIR}/tests/unit/lsp/test_lsp_config.c")
set(original_config_helpers "${CMAKE_CURRENT_BINARY_DIR}/original_config_helpers.c")
add_custom_command(OUTPUT "${original_config_helpers}"
    COMMAND "${Python3_EXECUTABLE}" -X utf8 "${CMAKE_CURRENT_LIST_DIR}/generate_subset.py"
        "${original_config_source}" "${CMAKE_CURRENT_LIST_DIR}/test_original.c" "${original_config_helpers}"
    DEPENDS "${original_config_source}" "${CMAKE_CURRENT_LIST_DIR}/test_original.c"
        "${CMAKE_CURRENT_LIST_DIR}/generate_subset.py" VERBATIM)
add_executable(test_lsp_original_config_helpers "${original_config_helpers}"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_config.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_source_config.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_ignore.c")
target_link_libraries(test_lsp_original_config_helpers PRIVATE xray_lsp_document_open)
target_compile_features(test_lsp_original_config_helpers PRIVATE c_std_11)
if(MSVC)
    target_compile_options(test_lsp_original_config_helpers PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_lsp_original_config_helpers PRIVATE -Wall -Wextra -Werror)
endif()
if(WIN32)
    target_link_libraries(test_lsp_original_config_helpers PRIVATE bcrypt)
endif()
add_test(NAME test_lsp_original_config_helpers COMMAND "${Python3_EXECUTABLE}" -X utf8
    "${CMAKE_CURRENT_LIST_DIR}/verify_original.py"
    "$<TARGET_FILE:test_lsp_original_config_helpers>"
    "${original_config_source}" "${original_config_helpers}")
set_tests_properties(test_lsp_original_config_helpers PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE LABELS "unit;lsp;configuration")
