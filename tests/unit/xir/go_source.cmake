add_executable(test_xir_go_source xir/test_xir_go_source.c)
target_link_libraries(test_xir_go_source PRIVATE xray_xir_source)
target_compile_definitions(test_xir_go_source PRIVATE
    XR_GO_SOURCE_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_go_source"
    XR_SOURCE_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
if(MSVC)
    target_compile_options(test_xir_go_source PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_go_source PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_go_source COMMAND test_xir_go_source)
set_tests_properties(test_xir_go_source PROPERTIES LABELS "unit;xir;source;task;ownership;compiler" TIMEOUT 300)
foreach(case integer string generic context_ordinary context_go context_existing
    context_spawn grouped import_default const_state local_storage unknown_task value_error shadow_class
    context_match context_match_block context_match_existing)
    add_test(NAME test_xir_go_source_compiler_${case} COMMAND test_xir_go_source --compiler ${case})
    set_tests_properties(test_xir_go_source_compiler_${case} PROPERTIES
        LABELS "unit;xir;source;task;ownership;compiler;compiler-fault" TIMEOUT 300 COST 3)
endforeach()
