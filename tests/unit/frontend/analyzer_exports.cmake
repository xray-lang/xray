add_executable(test_analyzer_exports
    "${CMAKE_CURRENT_LIST_DIR}/test_analyzer_exports.c"
    "${PROJECT_SOURCE_DIR}/src/frontend/analyzer/xanalyzer_storage.c"
    "${PROJECT_SOURCE_DIR}/src/frontend/analyzer/xanalyzer_scope_storage.c"
    "${PROJECT_SOURCE_DIR}/src/frontend/analyzer/xanalyzer_symbol_cleanup.c"
    "${PROJECT_SOURCE_DIR}/src/frontend/analyzer/xanalyzer_enum_storage.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/value/xtype_tid.c"
    "${PROJECT_SOURCE_DIR}/src/frontend/analyzer/xanalyzer_signature.c"
    "${PROJECT_SOURCE_DIR}/src/frontend/analyzer/xanalyzer_exports.c"
    "${PROJECT_SOURCE_DIR}/src/frontend/analyzer/xanalyzer_diagnostics.c"
    "${PROJECT_SOURCE_DIR}/src/frontend/analyzer/xanalyzer_class_storage.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/value/xenum_layout.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/value/xstruct_layout.c"
    "${PROJECT_SOURCE_DIR}/src/base/xtarget_data_layout.c"
    "${PROJECT_SOURCE_DIR}/src/frontend/analyzer/xanalyzer_native_input.c"
    "${PROJECT_SOURCE_DIR}/src/frontend/analyzer/xanalyzer_vtable.c"
    "${PROJECT_SOURCE_DIR}/src/frontend/analyzer/xa_native_effect.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/value/xtype_pool.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/value/xtype.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/value/xtype_generic.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/value/xtype_format.c"
    "${PROJECT_SOURCE_DIR}/src/base/xhashmap.c"
    "${PROJECT_SOURCE_DIR}/src/base/xfileio.c"
    "${PROJECT_SOURCE_DIR}/src/base/xio_policy.c"
)
target_link_libraries(test_analyzer_exports PRIVATE xray_source_compile)
if(WIN32)
    target_sources(test_analyzer_exports PRIVATE "${PROJECT_SOURCE_DIR}/src/os/win/thread_win.c")
else()
    target_sources(test_analyzer_exports PRIVATE "${PROJECT_SOURCE_DIR}/src/os/unix/thread_unix.c")
    find_package(Threads REQUIRED)
    target_link_libraries(test_analyzer_exports PRIVATE Threads::Threads)
endif()
target_include_directories(test_analyzer_exports PRIVATE
    "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/src/base")
target_compile_features(test_analyzer_exports PRIVATE c_std_11)
if(MSVC)
    target_compile_definitions(test_analyzer_exports PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_options(test_analyzer_exports PRIVATE /W4 /WX /utf-8)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_analyzer_exports PRIVATE /experimental:c11atomics)
    endif()
else()
    target_compile_options(test_analyzer_exports PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME test_analyzer_exports COMMAND test_analyzer_exports)
set_tests_properties(test_analyzer_exports PROPERTIES
    TIMEOUT 120 PROCESSORS 1 LABELS "unit;compiler;analyzer;ownership;budget;resources")

target_compile_definitions(test_analyzer_exports PRIVATE
    XR_TEST_EXPORT_ROOT="${PROJECT_SOURCE_DIR}/tests/unit/frontend")
