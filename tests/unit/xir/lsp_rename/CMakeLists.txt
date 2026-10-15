if(TARGET test_lsp_source_rename)
    return()
endif()
add_executable(test_lsp_source_rename "${CMAKE_CURRENT_LIST_DIR}/test_rename.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_source_rename.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_source_bindings.c")
target_link_libraries(test_lsp_source_rename PRIVATE xray_lsp_document_open)
target_include_directories(test_lsp_source_rename PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/xir")
if(WIN32)
    target_link_libraries(test_lsp_source_rename PRIVATE bcrypt)
endif()
target_compile_features(test_lsp_source_rename PRIVATE c_std_11)
if(MSVC)
    target_compile_options(test_lsp_source_rename PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_lsp_source_rename PRIVATE -Wall -Wextra -Werror)
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME test_lsp_source_rename COMMAND "${Python3_EXECUTABLE}" -X utf8
    "${CMAKE_CURRENT_LIST_DIR}/test_rename.py" "$<TARGET_FILE:test_lsp_source_rename>" "${PROJECT_SOURCE_DIR}/stdlib")
set_tests_properties(test_lsp_source_rename PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 8 COST 290 LABELS "unit;lsp;ownership;metadata;allocation")
add_test(NAME test_lsp_source_rename_enum_required COMMAND "${Python3_EXECUTABLE}" -X utf8
    "${CMAKE_CURRENT_LIST_DIR}/test_rename.py" "$<TARGET_FILE:test_lsp_source_rename>" "${PROJECT_SOURCE_DIR}/stdlib" --enum-required)
set_tests_properties(test_lsp_source_rename_enum_required PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE LABELS "unit;lsp;source;required")
add_test(NAME test_lsp_source_rename_resource_probe COMMAND "${Python3_EXECUTABLE}" -X utf8
    "${CMAKE_CURRENT_LIST_DIR}/test_rename.py" "$<TARGET_FILE:test_lsp_source_rename>" "${PROJECT_SOURCE_DIR}/stdlib" --probe)
set_tests_properties(test_lsp_source_rename_resource_probe PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1 LABELS "unit;lsp;ownership;metadata;allocation")
add_test(NAME test_lsp_source_rename_normal COMMAND "${Python3_EXECUTABLE}" -X utf8
    "${CMAKE_CURRENT_LIST_DIR}/test_rename.py" "$<TARGET_FILE:test_lsp_source_rename>" "${PROJECT_SOURCE_DIR}/stdlib" --normal)
set_tests_properties(test_lsp_source_rename_normal PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1 LABELS "unit;lsp;source;ownership")
