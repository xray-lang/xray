add_executable(test_xir_tuple_whole_source_compile_owner xir/test_xir_tuple_whole_source_compile_owner.c)
target_link_libraries(test_xir_tuple_whole_source_compile_owner PRIVATE xray_xir_source_product)
target_compile_definitions(test_xir_tuple_whole_source_compile_owner PRIVATE
    XR_TUPLE_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_tuple/runtime"
    XR_TUPLE_STDLIB="${CMAKE_SOURCE_DIR}/stdlib")
if(MSVC)
    target_compile_options(test_xir_tuple_whole_source_compile_owner PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_tuple_whole_source_compile_owner PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_tuple_whole_source_compile_owner COMMAND test_xir_tuple_whole_source_compile_owner)
set_tests_properties(test_xir_tuple_whole_source_compile_owner PROPERTIES LABELS "unit;xir;tuple;source;metadata;ownership" TIMEOUT 300 COST 100)
