add_executable(test_analyzer_storage
    "${CMAKE_CURRENT_LIST_DIR}/test_analyzer_storage.c"
    "${PROJECT_SOURCE_DIR}/src/frontend/analyzer/xanalyzer_storage.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/value/xtype_pool.c"
    "${PROJECT_SOURCE_DIR}/src/base/xhashmap.c"
    "${PROJECT_SOURCE_DIR}/src/base/xio_policy.c")
target_link_libraries(test_analyzer_storage PRIVATE xray_compiler_parser)
target_include_directories(test_analyzer_storage PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_features(test_analyzer_storage PRIVATE c_std_11)
if(MSVC)
    target_compile_definitions(test_analyzer_storage PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_options(test_analyzer_storage PRIVATE /W4 /WX /utf-8)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_analyzer_storage PRIVATE /experimental:c11atomics)
    endif()
else()
    target_compile_options(test_analyzer_storage PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME test_analyzer_storage COMMAND test_analyzer_storage)
set_tests_properties(test_analyzer_storage PROPERTIES
    LABELS "unit;compiler;analyzer;ownership;budget;resources" TIMEOUT 120 PROCESSORS 1)
