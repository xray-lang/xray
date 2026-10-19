add_executable(test_xir_mixed_terminal_producer xir/test_xir_mixed_terminal_producer.c)
target_link_libraries(test_xir_mixed_terminal_producer PRIVATE xray_xir_source xray_xir_cgen)
target_compile_definitions(test_xir_mixed_terminal_producer PRIVATE
    XR_TERMINAL_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_mixed_terminals")
set(XIR_TERMINAL_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_mixed_terminals.c")
add_custom_command(OUTPUT "${XIR_TERMINAL_GENERATED}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_mixed_terminal_producer> "${XIR_TERMINAL_GENERATED}"
    DEPENDS test_xir_mixed_terminal_producer
        "${CMAKE_SOURCE_DIR}/tests/fixtures/xir_mixed_terminals/root.xr"
        "${CMAKE_SOURCE_DIR}/tests/fixtures/xir_mixed_terminals/callee.xr"
    VERBATIM)
add_executable(test_xir_mixed_terminals xir/test_xir_mixed_terminals.c "${XIR_TERMINAL_GENERATED}")
target_link_libraries(test_xir_mixed_terminals PRIVATE xray_xir_vm xray_xir_scalar)
foreach(terminal_target IN ITEMS test_xir_mixed_terminal_producer test_xir_mixed_terminals)
    set_target_properties(${terminal_target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${terminal_target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${terminal_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
foreach(terminal_owner_backend IN ITEMS 0 1 vm native)
    foreach(terminal_owner_mode IN ITEMS axes fi)
        add_test(NAME test_xir_mixed_terminal_owner_${terminal_owner_backend}_${terminal_owner_mode}
            COMMAND test_xir_mixed_terminals ${terminal_owner_backend} ${terminal_owner_mode})
        set_tests_properties(test_xir_mixed_terminal_owner_${terminal_owner_backend}_${terminal_owner_mode} PROPERTIES
            TIMEOUT 120 RUN_SERIAL TRUE COST 1
            LABELS "unit;xir;mixed;execution;ownership;instance;generated-c;lifecycle;${terminal_owner_mode}")
    endforeach()
endforeach()
foreach(root_native IN ITEMS 0 1)
    add_test(NAME test_xir_mixed_terminals_${root_native} COMMAND test_xir_mixed_terminals ${root_native})
    set_tests_properties(test_xir_mixed_terminals_${root_native} PROPERTIES
        TIMEOUT 120 RUN_SERIAL TRUE COST 1 LABELS "unit;xir;mixed;execution;ownership;instance;abi;generated-c;lifecycle")
endforeach()
foreach(terminal_backend IN ITEMS vm native)
    add_test(NAME test_xir_mixed_terminals_${terminal_backend} COMMAND test_xir_mixed_terminals ${terminal_backend})
    set_tests_properties(test_xir_mixed_terminals_${terminal_backend} PROPERTIES
        TIMEOUT 120 RUN_SERIAL TRUE COST 1 LABELS "unit;xir;mixed;execution;ownership;instance;abi;generated-c;lifecycle;${terminal_backend}")
endforeach()
