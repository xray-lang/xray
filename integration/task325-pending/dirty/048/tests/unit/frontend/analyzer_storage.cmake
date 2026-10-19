add_executable(test_analyzer_storage
    "${CMAKE_CURRENT_LIST_DIR}/test_analyzer_storage.c"
    "${PROJECT_SOURCE_DIR}/src/frontend/analyzer/xanalyzer_storage.c"
    "${PROJECT_SOURCE_DIR}/src/frontend/analyzer/xanalyzer_native_input.c"
    "${PROJECT_SOURCE_DIR}/src/frontend/analyzer/xanalyzer_vtable.c"
    "${PROJECT_SOURCE_DIR}/src/frontend/analyzer/xa_native_effect.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/value/xtype_pool.c"
    "${PROJECT_SOURCE_DIR}/src/base/xhashmap.c"
    "${PROJECT_SOURCE_DIR}/src/base/xfileio.c"
    "${PROJECT_SOURCE_DIR}/src/base/xio_policy.c")
configure_file("${CMAKE_CURRENT_LIST_DIR}/source_identity/right/same.xr"
    "${CMAKE_CURRENT_BINARY_DIR}/same.xr" COPYONLY)
target_compile_definitions(test_analyzer_storage PRIVATE
    XR_ANALYZER_SOURCE_FIXTURE="${CMAKE_CURRENT_LIST_DIR}/source_identity")
target_link_libraries(test_analyzer_storage PRIVATE xray_source_compile)
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

add_executable(test_analyzer_class_storage
    "${CMAKE_CURRENT_LIST_DIR}/test_analyzer_class_storage.c"
    "${PROJECT_SOURCE_DIR}/src/frontend/analyzer/xanalyzer_class_storage.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/value/xstruct_layout.c"
    "${PROJECT_SOURCE_DIR}/src/base/xtarget_data_layout.c"
    "${PROJECT_SOURCE_DIR}/src/frontend/analyzer/xanalyzer_vtable.c")
target_link_libraries(test_analyzer_class_storage PRIVATE xray_source_compile)
target_include_directories(test_analyzer_class_storage PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_features(test_analyzer_class_storage PRIVATE c_std_11)
if(MSVC)
    target_compile_options(test_analyzer_class_storage PRIVATE /W4 /WX /utf-8)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_analyzer_class_storage PRIVATE /experimental:c11atomics)
    endif()
else()
    target_compile_options(test_analyzer_class_storage PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME test_analyzer_class_storage COMMAND test_analyzer_class_storage)
set_tests_properties(test_analyzer_class_storage PROPERTIES
    LABELS "unit;compiler;analyzer;ownership;budget;resources" TIMEOUT 120 PROCESSORS 1)
