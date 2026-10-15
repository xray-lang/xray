if(TARGET test_lsp_source_links)
    return()
endif()
add_executable(test_lsp_source_links "${CMAKE_CURRENT_LIST_DIR}/test_links.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_source_links.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_source_imports.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_links.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_index_pool.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_source_index.c")
if(WIN32)
    target_sources(test_lsp_source_links PRIVATE "${PROJECT_SOURCE_DIR}/src/os/win/thread_win.c")
    target_link_libraries(test_lsp_source_links PRIVATE bcrypt)
else()
    target_sources(test_lsp_source_links PRIVATE "${PROJECT_SOURCE_DIR}/src/os/unix/thread_unix.c")
endif()
target_link_libraries(test_lsp_source_links PRIVATE xray_lsp_document_open)
target_include_directories(test_lsp_source_links PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/xir" "${PROJECT_SOURCE_DIR}/src/base")
target_compile_features(test_lsp_source_links PRIVATE c_std_11)
if(MSVC)
    target_compile_options(test_lsp_source_links PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_lsp_source_links PRIVATE -Wall -Wextra -Werror)
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME test_lsp_source_links COMMAND "${Python3_EXECUTABLE}" -X utf8
    "${CMAKE_CURRENT_LIST_DIR}/test_links.py" "$<TARGET_FILE:test_lsp_source_links>" "${PROJECT_SOURCE_DIR}/stdlib")
set_tests_properties(test_lsp_source_links PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE LABELS "unit;lsp;ownership;metadata;allocation")
