# Both Source and Checked Catalog consumers execute independent state oracles.
set(XIR_LIBRARY_STATE_TYPED_VALUES_SOURCE ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_library_state_typed_values_source.xrc)
set(XIR_LIBRARY_STATE_TYPED_VALUES_CATALOG ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_library_state_typed_values_catalog.xrc)
add_library(xir_library_state_typed_values_source_runtime OBJECT xir/test_xir_library_state_typed_values_execution.c)
target_compile_definitions(xir_library_state_typed_values_source_runtime PRIVATE CONSUMER_KIND=3)
target_link_libraries(xir_library_state_typed_values_source_runtime PRIVATE xray_xir_vm)
add_executable(test_xir_library_state_typed_values_source xir/test_xir_library_state_typed_values_source.c
    $<TARGET_OBJECTS:xir_library_state_typed_values_source_runtime>)
target_link_libraries(test_xir_library_state_typed_values_source PRIVATE xray_xir_source xray_xir_vm)
target_compile_definitions(test_xir_library_state_typed_values_source PRIVATE
    XR_SOURCE_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_library_state_typed_values")
file(GLOB XIR_LIBRARY_STATE_TYPED_VALUES_FIXTURES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/tests/fixtures/xir_library_state_typed_values/*.xr")
add_custom_command(OUTPUT ${XIR_LIBRARY_STATE_TYPED_VALUES_SOURCE} ${XIR_LIBRARY_STATE_TYPED_VALUES_CATALOG}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND $<TARGET_FILE:test_xir_library_state_typed_values_source> --write-checked ${XIR_LIBRARY_STATE_TYPED_VALUES_SOURCE} ${XIR_LIBRARY_STATE_TYPED_VALUES_CATALOG}
    DEPENDS test_xir_library_state_typed_values_source ${XIR_LIBRARY_STATE_TYPED_VALUES_FIXTURES}
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_library_state_typed_values_runtime.h
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_library_state_typed_values_source_cases.h
    VERBATIM)
add_executable(test_xir_library_state_typed_values_packet xir/test_xir_library_state_typed_values_execution.c)
target_compile_definitions(test_xir_library_state_typed_values_packet PRIVATE CONSUMER_KIND=0)
target_link_libraries(test_xir_library_state_typed_values_packet PRIVATE xray_xir_vm xray_xir_cgen)
set(XIR_LIBRARY_STATE_TYPED_VALUES_TARGETS xir_library_state_typed_values_source_runtime test_xir_library_state_typed_values_source test_xir_library_state_typed_values_packet)
foreach(state_flow IN ITEMS source catalog)
    if(state_flow STREQUAL "source")
        set(state_checked ${XIR_LIBRARY_STATE_TYPED_VALUES_SOURCE})
    else()
        set(state_checked ${XIR_LIBRARY_STATE_TYPED_VALUES_CATALOG})
    endif()
    set(state_generated ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_library_state_typed_values_${state_flow}.c)
    add_custom_command(OUTPUT ${state_generated}
        COMMAND $<TARGET_FILE:test_xir_library_state_typed_values_packet> --write-c ${state_checked} ${state_generated}
        DEPENDS test_xir_library_state_typed_values_packet ${state_checked}
        VERBATIM)
    add_test(NAME test_xir_library_state_typed_values_${state_flow}_packet COMMAND test_xir_library_state_typed_values_packet
        ${state_checked} ${CMAKE_CURRENT_BINARY_DIR}/library-state-typed_values-${state_flow}-packet-test.c)
    foreach(state_mode IN ITEMS native mixed)
        set(state_target test_xir_library_state_typed_values_${state_flow}_${state_mode})
        add_executable(${state_target} xir/test_xir_library_state_typed_values_execution.c ${state_generated})
        target_link_libraries(${state_target} PRIVATE xray_xir_vm)
        if(state_mode STREQUAL "mixed")
            target_compile_definitions(${state_target} PRIVATE CONSUMER_KIND=2)
        else()
            target_compile_definitions(${state_target} PRIVATE CONSUMER_KIND=1)
        endif()
        list(APPEND XIR_LIBRARY_STATE_TYPED_VALUES_TARGETS ${state_target})
        add_test(NAME ${state_target} COMMAND ${state_target})
    endforeach()
    set_tests_properties(test_xir_library_state_typed_values_${state_flow}_packet
        test_xir_library_state_typed_values_${state_flow}_native test_xir_library_state_typed_values_${state_flow}_mixed
        PROPERTIES LABELS "unit;xir;execution;ownership;abi;budget" TIMEOUT 600 RUN_SERIAL TRUE)
endforeach()
foreach(state_target IN LISTS XIR_LIBRARY_STATE_TYPED_VALUES_TARGETS)
    if(MSVC)
        target_compile_options(${state_target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${state_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_xir_library_state_typed_values_source COMMAND test_xir_library_state_typed_values_source)
set_tests_properties(test_xir_library_state_typed_values_source PROPERTIES
    LABELS "unit;xir;execution;ownership;abi;budget" TIMEOUT 1200 RUN_SERIAL TRUE COST 120)
