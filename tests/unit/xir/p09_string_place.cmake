

add_executable(test_xir_p09_string_checked "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_p09_string_checked.c")
target_link_libraries(test_xir_p09_string_checked PRIVATE xray_xir_source)
set_target_properties(test_xir_p09_string_checked PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_p09_string_checked PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_xir_p09_string_checked PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_p09_string_checked COMMAND test_xir_p09_string_checked)
set_tests_properties(test_xir_p09_string_checked PROPERTIES TIMEOUT 120 LABELS "unit;xir;string;ownership;rejection;abi")

add_test(NAME test_xir_p09_sizing75_vectors COMMAND ${XRAY_PYTHON}
    "${PROJECT_SOURCE_DIR}/tests/unit/xir/derive_hash75_vectors.py"
)
set_tests_properties(test_xir_p09_sizing75_vectors PROPERTIES TIMEOUT 120 LABELS "unit;xir;wire;independent;compiler")
