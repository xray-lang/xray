# Complete Source constants use the same sole compiler and scalar interpreter.
add_executable(test_xir_source_comptime "${CMAKE_CURRENT_LIST_DIR}/test_xir_source_comptime.c")
target_link_libraries(test_xir_source_comptime PRIVATE xray_xir_source xray_xir_vm)
set_target_properties(test_xir_source_comptime PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_source_comptime PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_xir_source_comptime PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_source_comptime COMMAND test_xir_source_comptime)
set_tests_properties(test_xir_source_comptime PROPERTIES LABELS "unit;xir;source;ctfe;ownership" TIMEOUT 120 PROCESSORS 1 RUN_SERIAL TRUE)
add_test(NAME test_xir_source_comptime_resources COMMAND test_xir_source_comptime --resources)
set_tests_properties(test_xir_source_comptime_resources PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;compiler-fault;resources" TIMEOUT 600 PROCESSORS 1 RUN_SERIAL TRUE)

add_test(NAME test_xir_source_comptime_locals_resources COMMAND test_xir_source_comptime --local-resources)
set_tests_properties(test_xir_source_comptime_locals_resources PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;compiler-fault;resources;locals" TIMEOUT 600 PROCESSORS 1 RUN_SERIAL TRUE)

add_test(NAME test_xir_source_comptime_unit_resources COMMAND test_xir_source_comptime --unit-resources)
set_tests_properties(test_xir_source_comptime_unit_resources PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;compiler-fault;resources" TIMEOUT 600 RUN_SERIAL TRUE)

add_test(NAME test_xir_source_comptime_rune_resources COMMAND test_xir_source_comptime --rune-resources)
set_tests_properties(test_xir_source_comptime_rune_resources PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;compiler-fault;resources;rune" TIMEOUT 600 RUN_SERIAL TRUE)

add_test(NAME test_xir_source_comptime_cell_resources COMMAND test_xir_source_comptime --cell-resources)
set_tests_properties(test_xir_source_comptime_cell_resources PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;compiler-fault;resources;cell" TIMEOUT 600 RUN_SERIAL TRUE)

add_test(NAME test_xir_source_comptime_string_resources COMMAND test_xir_source_comptime --string-resources)
set_tests_properties(test_xir_source_comptime_string_resources PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;compiler-fault;resources;string" TIMEOUT 600 RUN_SERIAL TRUE)

add_test(NAME test_xir_source_comptime_string_read_resources COMMAND test_xir_source_comptime --string-read-resources)
set_tests_properties(test_xir_source_comptime_string_read_resources PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;compiler-fault;resources;string" TIMEOUT 600 RUN_SERIAL TRUE COST 166)

add_test(NAME test_xir_source_comptime_tuple_resources COMMAND test_xir_source_comptime --tuple-resources)
set_tests_properties(test_xir_source_comptime_tuple_resources PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;compiler-fault;resources;tuple" TIMEOUT 600 RUN_SERIAL TRUE)

add_test(NAME test_xir_source_comptime_float_resources COMMAND test_xir_source_comptime --float-resources)
set_tests_properties(test_xir_source_comptime_float_resources PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;compiler-fault;resources;float" TIMEOUT 600 RUN_SERIAL TRUE)

add_test(NAME test_xir_source_comptime_nullable_resources COMMAND test_xir_source_comptime --nullable-resources)
set_tests_properties(test_xir_source_comptime_nullable_resources PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;compiler-fault;resources;nullable" TIMEOUT 600 RUN_SERIAL TRUE)

add_test(NAME test_xir_source_comptime_array_resources COMMAND test_xir_source_comptime --array-resources)
set_tests_properties(test_xir_source_comptime_array_resources PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;compiler-fault;resources;array" TIMEOUT 600 RUN_SERIAL TRUE)

add_test(NAME test_xir_source_comptime_array_equal_resources COMMAND test_xir_source_comptime --array-equal-resources)
set_tests_properties(test_xir_source_comptime_array_equal_resources PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;compiler-fault;resources;array;equal" TIMEOUT 600 RUN_SERIAL TRUE)

add_test(NAME test_xir_source_comptime_array_path_resources COMMAND test_xir_source_comptime --array-path-resources)
set_tests_properties(test_xir_source_comptime_array_path_resources PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;compiler-fault;resources;array;path" TIMEOUT 600 RUN_SERIAL TRUE)

add_test(NAME test_xir_source_comptime_text_resources COMMAND test_xir_source_comptime --text-resources)
set_tests_properties(test_xir_source_comptime_text_resources PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;compiler-fault;resources;text" TIMEOUT 600 RUN_SERIAL TRUE)

add_test(NAME test_xir_source_comptime_nominal COMMAND test_xir_source_comptime --nominal)
set_tests_properties(test_xir_source_comptime_nominal PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;nominal" TIMEOUT 30)
add_test(NAME test_xir_source_comptime_nominal_resources COMMAND test_xir_source_comptime --nominal-resources)
set_tests_properties(test_xir_source_comptime_nominal_resources PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;compiler-fault;resources;nominal" TIMEOUT 600 RUN_SERIAL TRUE)

add_test(NAME test_xir_source_comptime_lexical COMMAND test_xir_source_comptime --lexical)
set_tests_properties(test_xir_source_comptime_lexical PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;lexical" TIMEOUT 30)
add_test(NAME test_xir_source_comptime_lexical_resources COMMAND test_xir_source_comptime --lexical-resources)
set_tests_properties(test_xir_source_comptime_lexical_resources PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;compiler-fault;resources;lexical" TIMEOUT 600 RUN_SERIAL TRUE)

add_test(NAME test_xir_source_comptime_function COMMAND test_xir_source_comptime --function)
set_tests_properties(test_xir_source_comptime_function PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;function" TIMEOUT 30)
add_test(NAME test_xir_source_comptime_function_resources COMMAND test_xir_source_comptime --function-resources)
set_tests_properties(test_xir_source_comptime_function_resources PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;compiler-fault;resources;function" TIMEOUT 600 RUN_SERIAL TRUE)

add_test(NAME test_xir_source_comptime_cleanup COMMAND test_xir_source_comptime --cleanup)
set_tests_properties(test_xir_source_comptime_cleanup PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;cleanup" TIMEOUT 30)
add_test(NAME test_xir_source_comptime_cleanup_resources COMMAND test_xir_source_comptime --cleanup-resources)
set_tests_properties(test_xir_source_comptime_cleanup_resources PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;compiler-fault;resources;cleanup" TIMEOUT 600 RUN_SERIAL TRUE)

add_test(NAME test_xir_source_comptime_projection COMMAND test_xir_source_comptime --projection)
set_tests_properties(test_xir_source_comptime_projection PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;projection" TIMEOUT 30)
add_test(NAME test_xir_source_comptime_projection_resources COMMAND test_xir_source_comptime --projection-resources)
set_tests_properties(test_xir_source_comptime_projection_resources PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;compiler-fault;resources;projection" TIMEOUT 600 RUN_SERIAL TRUE)

add_test(NAME test_xir_source_comptime_error COMMAND test_xir_source_comptime --error)
set_tests_properties(test_xir_source_comptime_error PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;error" TIMEOUT 30)
add_test(NAME test_xir_source_comptime_error_resources COMMAND test_xir_source_comptime --error-resources)
set_tests_properties(test_xir_source_comptime_error_resources PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;compiler-fault;resources;error" TIMEOUT 600 RUN_SERIAL TRUE)
