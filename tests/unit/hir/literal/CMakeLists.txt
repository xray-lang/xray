add_executable(test_hir_numeric_literal test_literal.c)
target_link_libraries(test_hir_numeric_literal PRIVATE xray_xir_source xray_xir_vm)
set_target_properties(test_hir_numeric_literal PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_hir_numeric_literal PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_hir_numeric_literal PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME hir_numeric_literal COMMAND test_hir_numeric_literal)
set_tests_properties(hir_numeric_literal PROPERTIES TIMEOUT 60 LABELS "unit;hir;literals;ownership")
if(WIN32)
    add_test(NAME hir_numeric_literal_cli COMMAND ${XRAY_PYTHON} -X utf8
        "${CMAKE_CURRENT_SOURCE_DIR}/test_literal_cli.py"
        --root "${PROJECT_SOURCE_DIR}" --binary $<TARGET_FILE:xray>)
    set_tests_properties(hir_numeric_literal_cli PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE
        LABELS "unit;hir;literals;cli;native")
endif()
