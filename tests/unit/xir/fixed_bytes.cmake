add_executable(test_xir_fixed_bytes xir/test_xir_fixed_bytes.c xir/fixed_bytes_value_alloc.c)
target_link_libraries(test_xir_fixed_bytes PRIVATE xray_xir_scalar)
set_target_properties(test_xir_fixed_bytes PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(test_xir_fixed_bytes PRIVATE /W4 /WX /utf-8)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_xir_fixed_bytes PRIVATE /experimental:c11atomics)
    endif()
else()
    target_compile_options(test_xir_fixed_bytes PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME xir_fixed_bytes COMMAND test_xir_fixed_bytes)
set_tests_properties(xir_fixed_bytes PROPERTIES TIMEOUT 60 LABELS "unit;xir;fixed;ownership")



set(XIR_FIXED_BYTES_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/fixed_bytes.c)
