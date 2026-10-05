add_executable(test_xir_tuple_destructure_mixed
    xir/test_xir_tuple_destructure_mixed.c ${XIR_TUPLE_DESTRUCTURE_GENERATED})
target_link_libraries(test_xir_tuple_destructure_mixed PRIVATE xray_xir_vm xray_xir_cgen)
if(MSVC)
    target_compile_options(test_xir_tuple_destructure_mixed PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_tuple_destructure_mixed PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_tuple_destructure_mixed COMMAND test_xir_tuple_destructure_mixed)
set_tests_properties(test_xir_tuple_destructure_mixed
    PROPERTIES LABELS "unit;xir;tuple;mixed;ownership;execution" TIMEOUT 180)
