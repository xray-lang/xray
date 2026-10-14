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
