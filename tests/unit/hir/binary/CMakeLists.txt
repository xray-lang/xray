add_executable(test_hir_binary test_binary.c)
target_link_libraries(test_hir_binary PRIVATE xray_xir_source xray_xir_vm)
set_target_properties(test_hir_binary PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_hir_binary PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_hir_binary PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME hir_binary COMMAND test_hir_binary)
set_tests_properties(hir_binary PROPERTIES TIMEOUT 60 LABELS "unit;hir;types;ownership")
add_test(NAME source_binary_sdk COMMAND test_source_sdk_consumer --binary
    "${PROJECT_BINARY_DIR}/tests/unit/source-sdk-empty-stdlib")
set_tests_properties(source_binary_sdk PROPERTIES TIMEOUT 120 LABELS "unit;hir;api;sdk;ownership")
if(WIN32)
    add_test(NAME hir_binary_cli COMMAND ${XRAY_PYTHON} -X utf8
        "${CMAKE_CURRENT_SOURCE_DIR}/test_binary_cli.py"
        --root "${PROJECT_SOURCE_DIR}" --binary $<TARGET_FILE:xray>)
    set_tests_properties(hir_binary_cli PROPERTIES TIMEOUT 240 RUN_SERIAL TRUE
        LABELS "unit;hir;types;cli;native")
endif()
