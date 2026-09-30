# Direct source-owned Checked executes after producer destruction.
# Packet VM and generated native/mixed consumers link no source, parser or AST.
# One frozen Checked packet feeds those three independent source-free executions.
set(XIR_UNIT_SLOTS_CHECKED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_unit_slots.xrc)
set(XIR_UNIT_SLOTS_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_unit_slots.c)
add_xray_bootstrap_executable(test_xir_unit_slots_source xir/test_xir_unit_slots_source.c)
target_sources(test_xir_unit_slots_source PRIVATE xir/xir_unit_slots_source_runtime.c)
target_link_libraries(test_xir_unit_slots_source PRIVATE xray_xir_source xray_xir_vm)
target_compile_definitions(test_xir_unit_slots_source PRIVATE
    XR_SOURCE_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_unit_slots"
    XR_SOURCE_STDLIB="${CMAKE_SOURCE_DIR}/stdlib")
add_custom_command(OUTPUT ${XIR_UNIT_SLOTS_CHECKED}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND $<TARGET_FILE:test_xir_unit_slots_source> ${XIR_UNIT_SLOTS_CHECKED}
    DEPENDS test_xir_unit_slots_source
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_unit_slots/root.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_unit_slots/base.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_unit_slots/worker.xr
    VERBATIM)
add_executable(test_xir_unit_slots_packet xir/test_xir_unit_slots_packet.c ${XIR_UNIT_SLOTS_CHECKED})
target_link_libraries(test_xir_unit_slots_packet PRIVATE xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_xir_unit_slots_packet PRIVATE XR_CHECKED_FIXTURE="${XIR_UNIT_SLOTS_CHECKED}")
add_custom_command(OUTPUT ${XIR_UNIT_SLOTS_GENERATED}
    COMMAND $<TARGET_FILE:test_xir_unit_slots_packet> ${XIR_UNIT_SLOTS_GENERATED}
    DEPENDS test_xir_unit_slots_packet ${XIR_UNIT_SLOTS_CHECKED}
    VERBATIM)
foreach(unit_slots_mode IN ITEMS native mixed)
    add_executable(test_xir_unit_slots_${unit_slots_mode} xir/test_xir_unit_slots_execution.c ${XIR_UNIT_SLOTS_GENERATED})
    target_link_libraries(test_xir_unit_slots_${unit_slots_mode} PRIVATE xray_xir_vm)
    if(unit_slots_mode STREQUAL "mixed")
        target_compile_definitions(test_xir_unit_slots_${unit_slots_mode} PRIVATE XR_UNIT_SLOTS_MIXED=1)
    else()
        target_compile_definitions(test_xir_unit_slots_${unit_slots_mode} PRIVATE XR_UNIT_SLOTS_MIXED=0)
    endif()
endforeach()
foreach(unit_slots_test IN ITEMS test_xir_unit_slots_source test_xir_unit_slots_packet test_xir_unit_slots_native test_xir_unit_slots_mixed)
    if(MSVC)
        target_compile_options(${unit_slots_test} PRIVATE /W4 /WX)
    else()
        target_compile_options(${unit_slots_test} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${unit_slots_test} COMMAND ${unit_slots_test})
    set_tests_properties(${unit_slots_test} PROPERTIES LABELS "unit;xir;execution;ownership;abi" TIMEOUT 300)
endforeach()
set_tests_properties(test_xir_unit_slots_native test_xir_unit_slots_mixed PROPERTIES TIMEOUT 600)
