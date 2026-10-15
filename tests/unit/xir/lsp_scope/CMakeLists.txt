if(TARGET test_lsp_source_scope)
    return()
endif()
add_executable(test_lsp_source_scope "${CMAKE_CURRENT_LIST_DIR}/test_scope.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_source_scope.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_source_binding_lenses.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_navigation.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_ignore.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_index_scan.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_index_pool.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_source_index.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_source_bindings.c"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_source_rename.c")
target_link_libraries(test_lsp_source_scope PRIVATE xray_lsp_document_open xray_lsp_hierarchy_handlers)
if(WIN32)
    target_sources(test_lsp_source_scope PRIVATE "${PROJECT_SOURCE_DIR}/src/os/win/thread_win.c")
    target_link_libraries(test_lsp_source_scope PRIVATE bcrypt)
else()
    target_sources(test_lsp_source_scope PRIVATE "${PROJECT_SOURCE_DIR}/src/os/unix/thread_unix.c")
endif()
target_include_directories(test_lsp_source_scope PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/xir" "${PROJECT_SOURCE_DIR}/src/base")
target_compile_features(test_lsp_source_scope PRIVATE c_std_11)
if(MSVC)
    target_compile_options(test_lsp_source_scope PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_lsp_source_scope PRIVATE -Wall -Wextra -Werror)
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME test_lsp_source_scope COMMAND "${Python3_EXECUTABLE}" -X utf8
    "${CMAKE_CURRENT_LIST_DIR}/test_scope.py" "$<TARGET_FILE:test_lsp_source_scope>" "${PROJECT_SOURCE_DIR}/stdlib")
set_tests_properties(test_lsp_source_scope PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE LABELS "unit;lsp;ownership;metadata;allocation")
add_test(NAME test_lsp_source_scope_rename COMMAND "${Python3_EXECUTABLE}" -X utf8
    "${CMAKE_CURRENT_LIST_DIR}/test_scope.py" "$<TARGET_FILE:test_lsp_source_scope>" "${PROJECT_SOURCE_DIR}/stdlib" --rename)
set_tests_properties(test_lsp_source_scope_rename PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE COST 290 LABELS "unit;lsp;source;required;allocation")
add_test(NAME test_lsp_source_scope_code_lens COMMAND "${Python3_EXECUTABLE}" -X utf8
    "${CMAKE_CURRENT_LIST_DIR}/test_scope.py" "$<TARGET_FILE:test_lsp_source_scope>" "${PROJECT_SOURCE_DIR}/stdlib" --lens)
set_tests_properties(test_lsp_source_scope_code_lens PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE COST 290 LABELS "unit;lsp;source;required;allocation")
add_test(NAME test_lsp_source_binding_lenses COMMAND "${Python3_EXECUTABLE}" -X utf8
    "${CMAKE_CURRENT_LIST_DIR}/test_scope.py" "$<TARGET_FILE:test_lsp_source_scope>" "${PROJECT_SOURCE_DIR}/stdlib" --binding-lenses)
set_tests_properties(test_lsp_source_binding_lenses PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE COST 290 LABELS "unit;lsp;source;required;allocation")
set(scope_normal_modes normal rename-normal lens-normal binding-lenses-normal)
foreach(mode IN LISTS scope_normal_modes)
    add_test(NAME test_lsp_source_scope_${mode} COMMAND "${Python3_EXECUTABLE}" -X utf8
        "${CMAKE_CURRENT_LIST_DIR}/test_scope.py" "$<TARGET_FILE:test_lsp_source_scope>"
        "${PROJECT_SOURCE_DIR}/stdlib" --${mode})
    set_tests_properties(test_lsp_source_scope_${mode} PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE LABELS "unit;lsp;source;ownership;metadata")
endforeach()
