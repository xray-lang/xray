# One Source graph produces the packet shared by every execution consumer.
set(XIR_DEFAULT_GAP_CHECKED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_default_gaps.xrc)
set(XIR_DEFAULT_GAP_C ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_default_gaps.c)
add_xray_bootstrap_executable(test_xir_default_gap_source xir/test_xir_default_gap_execution.c)
target_link_libraries(test_xir_default_gap_source PRIVATE xray_xir_source xray_xir_vm)
target_compile_definitions(test_xir_default_gap_source PRIVATE XR_DEFAULT_GAP_MODE=0
    XR_SOURCE_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_default_gaps")
add_custom_command(OUTPUT ${XIR_DEFAULT_GAP_CHECKED}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND $<TARGET_FILE:test_xir_default_gap_source> ${XIR_DEFAULT_GAP_CHECKED} unused
    DEPENDS test_xir_default_gap_source
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_default_gaps/root.xr
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_default_gap_runtime.h VERBATIM)
add_executable(test_xir_default_gap_packet xir/test_xir_default_gap_execution.c ${XIR_DEFAULT_GAP_CHECKED})
target_link_libraries(test_xir_default_gap_packet PRIVATE xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_xir_default_gap_packet PRIVATE XR_DEFAULT_GAP_MODE=1)
add_custom_command(OUTPUT ${XIR_DEFAULT_GAP_C}
    COMMAND $<TARGET_FILE:test_xir_default_gap_packet> ${XIR_DEFAULT_GAP_CHECKED} ${XIR_DEFAULT_GAP_C}
    DEPENDS test_xir_default_gap_packet ${XIR_DEFAULT_GAP_CHECKED} VERBATIM)
add_executable(test_xir_default_gap_native xir/test_xir_default_gap_execution.c ${XIR_DEFAULT_GAP_C})
target_link_libraries(test_xir_default_gap_native PRIVATE xray_xir_scalar)
target_compile_definitions(test_xir_default_gap_native PRIVATE XR_DEFAULT_GAP_MODE=2)
add_executable(test_xir_default_gap_mixed xir/test_xir_default_gap_execution.c ${XIR_DEFAULT_GAP_C})
target_link_libraries(test_xir_default_gap_mixed PRIVATE xray_xir_vm)
target_compile_definitions(test_xir_default_gap_mixed PRIVATE XR_DEFAULT_GAP_MODE=3)
foreach(gap_target IN ITEMS source packet native mixed)
    if(MSVC)
        target_compile_options(test_xir_default_gap_${gap_target} PRIVATE /W4 /WX)
    else()
        target_compile_options(test_xir_default_gap_${gap_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_xir_default_gap_source COMMAND test_xir_default_gap_source ${CMAKE_CURRENT_BINARY_DIR}/default-gap-source-test.xrc unused)
add_test(NAME test_xir_default_gap_packet COMMAND test_xir_default_gap_packet ${XIR_DEFAULT_GAP_CHECKED} ${CMAKE_CURRENT_BINARY_DIR}/default-gap-packet-test.c)
add_test(NAME test_xir_default_gap_native COMMAND test_xir_default_gap_native)
add_test(NAME test_xir_default_gap_mixed COMMAND test_xir_default_gap_mixed)
set_tests_properties(test_xir_default_gap_source test_xir_default_gap_packet test_xir_default_gap_native test_xir_default_gap_mixed
    PROPERTIES LABELS "unit;xir;execution;ownership;abi")
