add_executable(test_xir_p09_string_append_probe "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_p09_string_append_probe.c")
target_link_libraries(test_xir_p09_string_append_probe PRIVATE xray_xir_source xray_xir_vm)
target_compile_definitions(test_xir_p09_string_append_probe PRIVATE
    XR_REFERENCE_FIXTURES="${PROJECT_SOURCE_DIR}/tests/unit/xir/p09_reference/fixtures"
    XR_REFERENCE_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
set_target_properties(test_xir_p09_string_append_probe PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_p09_string_append_probe PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_xir_p09_string_append_probe PRIVATE -Wall -Wextra -Werror)
endif()
foreach(count 64 512 4096 32768 1000000)
    add_test(NAME test_xir_p09_string_append_probe_${count} COMMAND test_xir_p09_string_append_probe ${count})
    set_tests_properties(test_xir_p09_string_append_probe_${count} PROPERTIES TIMEOUT 120
        LABELS "unit;xir;string;ownership;measurement")
endforeach()
