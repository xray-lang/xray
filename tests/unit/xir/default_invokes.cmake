# Suggested root-owned registration; one source graph, one packet, one emitted C.
set(XIR_DEFAULT_INVOKE_CHECKED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_default_invoke.xrc)
set(XIR_DEFAULT_INVOKE_C ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_default_invoke.c)
add_executable(test_xir_default_invoke_source xir/test_xir_default_invoke_execution.c)
target_link_libraries(test_xir_default_invoke_source PRIVATE xray_xir_source xray_xir_vm)
target_compile_definitions(test_xir_default_invoke_source PRIVATE XR_DEFAULT_INVOKE_MODE=0
    XR_SOURCE_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_default_invoke")
add_custom_command(OUTPUT ${XIR_DEFAULT_INVOKE_CHECKED}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND $<TARGET_FILE:test_xir_default_invoke_source> ${XIR_DEFAULT_INVOKE_CHECKED} unused
    DEPENDS test_xir_default_invoke_source ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_default_invoke/root.xr VERBATIM)
add_executable(test_xir_default_invoke_packet xir/test_xir_default_invoke_execution.c ${XIR_DEFAULT_INVOKE_CHECKED})
target_link_libraries(test_xir_default_invoke_packet PRIVATE xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_xir_default_invoke_packet PRIVATE XR_DEFAULT_INVOKE_MODE=1)
add_custom_command(OUTPUT ${XIR_DEFAULT_INVOKE_C}
    COMMAND $<TARGET_FILE:test_xir_default_invoke_packet> ${XIR_DEFAULT_INVOKE_CHECKED} ${XIR_DEFAULT_INVOKE_C}
    DEPENDS test_xir_default_invoke_packet ${XIR_DEFAULT_INVOKE_CHECKED} VERBATIM)
add_executable(test_xir_default_invoke_native xir/test_xir_default_invoke_execution.c ${XIR_DEFAULT_INVOKE_C})
target_link_libraries(test_xir_default_invoke_native PRIVATE xray_xir_scalar)
target_compile_definitions(test_xir_default_invoke_native PRIVATE XR_DEFAULT_INVOKE_MODE=2)
add_executable(test_xir_default_invoke_mixed xir/test_xir_default_invoke_execution.c ${XIR_DEFAULT_INVOKE_C})
target_link_libraries(test_xir_default_invoke_mixed PRIVATE xray_xir_vm)
target_compile_definitions(test_xir_default_invoke_mixed PRIVATE XR_DEFAULT_INVOKE_MODE=3)
foreach(gap_target IN ITEMS source packet native mixed)
    if(MSVC)
        target_compile_options(test_xir_default_invoke_${gap_target} PRIVATE /W4 /WX)
    else()
        target_compile_options(test_xir_default_invoke_${gap_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_xir_default_invoke_source COMMAND test_xir_default_invoke_source ${CMAKE_CURRENT_BINARY_DIR}/default-invoke-source-test.xrc unused)
add_test(NAME test_xir_default_invoke_packet COMMAND test_xir_default_invoke_packet ${XIR_DEFAULT_INVOKE_CHECKED} ${CMAKE_CURRENT_BINARY_DIR}/default-invoke-packet-test.c)
add_test(NAME test_xir_default_invoke_native COMMAND test_xir_default_invoke_native)
add_test(NAME test_xir_default_invoke_mixed COMMAND test_xir_default_invoke_mixed)
set_tests_properties(test_xir_default_invoke_source test_xir_default_invoke_packet test_xir_default_invoke_native test_xir_default_invoke_mixed
    PROPERTIES LABELS "unit;xir;execution;ownership;abi")

# Measured sanitizer scheduling only; execution inputs and timeout remain unchanged.
if(ENABLE_ASAN)
    set_tests_properties(test_xir_default_invoke_source PROPERTIES COST 189.251 PROCESSORS 1)
    set_tests_properties(test_xir_default_invoke_packet PROPERTIES COST 225.298 PROCESSORS 1)
    set_tests_properties(test_xir_default_invoke_mixed PROPERTIES COST 190.106 PROCESSORS 1)
endif()
