# Observe actual resource allocations independently of runtime allocations.
add_executable(test_xir_array_reorder_owner
    "${CMAKE_CURRENT_LIST_DIR}/test_xir_array_reorder_owner.c")
target_link_libraries(test_xir_array_reorder_owner PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_xir_array_reorder_owner PRIVATE
    XR_REORDER_OWNER_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_array_reorder_owner")
set_target_properties(test_xir_array_reorder_owner PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_array_reorder_owner PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_xir_array_reorder_owner PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_array_reorder_source_owner
    COMMAND $<TARGET_FILE:test_xir_array_reorder_owner> compiler)
add_test(NAME test_xir_array_reorder_return_owner
    COMMAND $<TARGET_FILE:test_xir_array_reorder_owner> runtime)
set_tests_properties(test_xir_array_reorder_source_owner test_xir_array_reorder_return_owner
    PROPERTIES LABELS "unit;xir;memory;ownership;budget;execution" TIMEOUT 120)

set(XIR_REORDER_OWNER_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_reorder_owner.c")
add_custom_command(OUTPUT "${XIR_REORDER_OWNER_C}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_array_reorder_owner> emit "${XIR_REORDER_OWNER_C}"
    DEPENDS test_xir_array_reorder_owner
        "${CMAKE_SOURCE_DIR}/tests/fixtures/xir_array_reorder_owner/root.xr"
    VERBATIM)
add_executable(test_xir_array_reorder_native_owner
    "${CMAKE_CURRENT_LIST_DIR}/test_xir_array_reorder_owner.c" "${XIR_REORDER_OWNER_C}")
target_link_libraries(test_xir_array_reorder_native_owner PRIVATE xray_xir_source xray_xir_scalar xray_xir_cgen)
target_compile_definitions(test_xir_array_reorder_native_owner PRIVATE XR_REORDER_NATIVE=1
    XR_REORDER_OWNER_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_array_reorder_owner")
set_target_properties(test_xir_array_reorder_native_owner PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_array_reorder_native_owner PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_xir_array_reorder_native_owner PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_array_reorder_native_return_owner
    COMMAND $<TARGET_FILE:test_xir_array_reorder_native_owner> runtime)
set_tests_properties(test_xir_array_reorder_native_return_owner
    PROPERTIES LABELS "unit;xir;memory;ownership;execution;aot" TIMEOUT 120)
