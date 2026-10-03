# Observe production runtime and compile-resource allocation boundaries separately.
add_executable(test_xir_string_allocation_owner
    "${CMAKE_CURRENT_LIST_DIR}/test_xir_string_allocation_owner.c")
target_link_libraries(test_xir_string_allocation_owner PRIVATE xray_xir_vm)
set_target_properties(test_xir_string_allocation_owner PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_string_allocation_owner PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_xir_string_allocation_owner PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_string_allocation_owner
    COMMAND $<TARGET_FILE:test_xir_string_allocation_owner>)
set_tests_properties(test_xir_string_allocation_owner PROPERTIES
    LABELS "unit;xir;memory;ownership;budget;execution" TIMEOUT 120)
