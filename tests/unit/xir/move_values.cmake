add_executable(test_xir_move_values "${CMAKE_CURRENT_LIST_DIR}/test_xir_move_values.c")
target_link_libraries(test_xir_move_values PRIVATE xray_xir_scalar)
set_target_properties(test_xir_move_values PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(test_xir_move_values PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_move_values PRIVATE -Wall -Wextra -Werror -pedantic-errors)
endif()
add_test(NAME test_xir_move_values COMMAND test_xir_move_values)
set_tests_properties(test_xir_move_values PROPERTIES TIMEOUT 120 LABELS "unit;xir;ownership")



set(move_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/move_values.c")
