add_executable(test_analyzer_scope_storage
    "${CMAKE_CURRENT_LIST_DIR}/test_analyzer_scope_storage.c"
    "${PROJECT_SOURCE_DIR}/src/frontend/analyzer/xanalyzer_symbol_cleanup.c"
    "${PROJECT_SOURCE_DIR}/src/frontend/analyzer/xanalyzer_enum_storage.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/value/xtype_tid.c"
    "${PROJECT_SOURCE_DIR}/src/frontend/analyzer/xanalyzer_signature.c"
    "${PROJECT_SOURCE_DIR}/src/frontend/analyzer/xanalyzer_class_storage.c"
    "${PROJECT_SOURCE_DIR}/src/frontend/analyzer/xanalyzer_vtable.c"
    "${PROJECT_SOURCE_DIR}/src/base/xhashmap.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/value/xenum_layout.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/value/xstruct_layout.c"
    "${PROJECT_SOURCE_DIR}/src/base/xtarget_data_layout.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/value/xtype_pool.c")
target_link_libraries(test_analyzer_scope_storage PRIVATE xray_source_compile)
target_include_directories(test_analyzer_scope_storage PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_features(test_analyzer_scope_storage PRIVATE c_std_11)
if(MSVC)
    target_compile_options(test_analyzer_scope_storage PRIVATE /W4 /WX /utf-8)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_analyzer_scope_storage PRIVATE /experimental:c11atomics)
    endif()
else()
    target_compile_options(test_analyzer_scope_storage PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME test_analyzer_scope_storage COMMAND test_analyzer_scope_storage)
set_tests_properties(test_analyzer_scope_storage PROPERTIES
    TIMEOUT 120 PROCESSORS 1 LABELS "unit;compiler;analyzer;ownership;budget;resources")
add_test(NAME test_analyzer_scope_storage_faults COMMAND test_analyzer_scope_storage --resources)
set_tests_properties(test_analyzer_scope_storage_faults PROPERTIES
    TIMEOUT 600 RUN_SERIAL TRUE COST 100 LABELS "unit;compiler;analyzer;ownership;compiler-faults")
