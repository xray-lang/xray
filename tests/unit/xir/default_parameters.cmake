# One Source graph produces the packet shared by every execution consumer.
set(XIR_DEFAULT_GAP_CHECKED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_default_gaps.xrc)
set(XIR_DEFAULT_GAP_C ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_default_gaps.c)
add_executable(test_xir_default_gap_source xir/test_xir_default_gap_execution.c)
target_link_libraries(test_xir_default_gap_source PRIVATE xray_xir_source xray_xir_vm)
target_compile_definitions(test_xir_default_gap_source PRIVATE XR_DEFAULT_GAP_MODE=0
    XR_SOURCE_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_default_gaps")
add_custom_command(OUTPUT ${XIR_DEFAULT_GAP_CHECKED}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND $<TARGET_FILE:test_xir_default_gap_source> --generate-only ${XIR_DEFAULT_GAP_CHECKED} unused
    DEPENDS test_xir_default_gap_source
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_default_gaps/root.xr
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_default_gap_runtime.h VERBATIM)
add_executable(test_xir_default_gap_packet xir/test_xir_default_gap_execution.c ${XIR_DEFAULT_GAP_CHECKED})
target_link_libraries(test_xir_default_gap_packet PRIVATE xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_xir_default_gap_packet PRIVATE XR_DEFAULT_GAP_MODE=1)
add_custom_command(OUTPUT ${XIR_DEFAULT_GAP_C}
    COMMAND $<TARGET_FILE:test_xir_default_gap_packet> --generate-only ${XIR_DEFAULT_GAP_CHECKED} ${XIR_DEFAULT_GAP_C}
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
if(WIN32)
    set(default_gap_mode 0)
    foreach(route IN ITEMS source packet native mixed)
        add_test(NAME test_xir_default_gap_${route} COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_LIST_DIR}/run_default_gap_compiler.py"
            --executable $<TARGET_FILE:test_xir_default_gap_${route}> --mode ${default_gap_mode}
            --packet ${XIR_DEFAULT_GAP_CHECKED} --generated-c ${XIR_DEFAULT_GAP_C} --workers 8
            --reports-root "${CMAKE_CURRENT_BINARY_DIR}/default-gap-partitions/${route}")
        set_tests_properties(test_xir_default_gap_${route} PROPERTIES PROCESSORS 8)
        math(EXPR default_gap_mode "${default_gap_mode} + 1")
    endforeach()
else()
add_test(NAME test_xir_default_gap_source COMMAND test_xir_default_gap_source ${CMAKE_CURRENT_BINARY_DIR}/default-gap-source-test.xrc unused)
add_test(NAME test_xir_default_gap_packet COMMAND test_xir_default_gap_packet ${XIR_DEFAULT_GAP_CHECKED} ${CMAKE_CURRENT_BINARY_DIR}/default-gap-packet-test.c)
add_test(NAME test_xir_default_gap_native COMMAND test_xir_default_gap_native)
add_test(NAME test_xir_default_gap_mixed COMMAND test_xir_default_gap_mixed)
endif()
add_test(NAME test_xir_default_gap_partition_protocol COMMAND ${Python3_EXECUTABLE}
    "${PROJECT_SOURCE_DIR}/tests/lib/tests/test_default_gap_partitions.py")
set_tests_properties(test_xir_default_gap_partition_protocol PROPERTIES TIMEOUT 30 LABELS "unit;xir;test-protocol;ownership")
set_tests_properties(test_xir_default_gap_source test_xir_default_gap_packet test_xir_default_gap_native test_xir_default_gap_mixed
    PROPERTIES LABELS "unit;xir;execution;ownership;abi")

add_test(NAME test_xir_default_gap_mixed_owned_recovery COMMAND test_xir_default_gap_mixed --owned-recovery)
set_tests_properties(test_xir_default_gap_mixed_owned_recovery PROPERTIES
    LABELS "unit;xir;execution;ownership;abi;owned-program" TIMEOUT 60)
foreach(binding_attack IN ITEMS artifact function)
    add_test(NAME test_xir_default_gap_mixed_owned_binding_${binding_attack}
        COMMAND test_xir_default_gap_mixed --owned-binding-${binding_attack})
    set_tests_properties(test_xir_default_gap_mixed_owned_binding_${binding_attack} PROPERTIES
        LABELS "unit;xir;execution;ownership;abi;owned-program" TIMEOUT 60)
endforeach()

add_test(NAME test_xir_default_gap_serial_compiler_control COMMAND test_xir_default_gap_native --serial-compiler-control)
set_tests_properties(test_xir_default_gap_serial_compiler_control PROPERTIES
    TIMEOUT 300 PROCESSORS 1 LABELS "unit;xir;ownership;shared-default-owner")
