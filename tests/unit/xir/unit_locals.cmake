# Direct source-owned Checked executes after producer destruction.
# Packet VM and generated native/mixed consumers link no source, parser or AST.
# One frozen Checked packet feeds those three independent source-free executions.
set(XIR_UNIT_LOCALS_CHECKED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_unit_locals.xrc)
set(XIR_UNIT_LOCALS_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_unit_locals.c)
add_executable(test_xir_unit_locals_source xir/test_xir_unit_locals_source.c)
target_sources(test_xir_unit_locals_source PRIVATE xir/xir_unit_locals_source_runtime.c)
target_link_libraries(test_xir_unit_locals_source PRIVATE xray_xir_source xray_xir_vm)
target_compile_definitions(test_xir_unit_locals_source PRIVATE
    XR_SOURCE_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_unit_locals"
    XR_SOURCE_STDLIB="${CMAKE_SOURCE_DIR}/stdlib")
add_custom_command(OUTPUT ${XIR_UNIT_LOCALS_CHECKED}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND $<TARGET_FILE:test_xir_unit_locals_source> --write-checked ${XIR_UNIT_LOCALS_CHECKED}
    DEPENDS test_xir_unit_locals_source
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_unit_locals/root.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_unit_locals/base.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_unit_locals/worker.xr
    VERBATIM)
add_executable(test_xir_unit_locals_packet xir/test_xir_unit_locals_packet.c ${XIR_UNIT_LOCALS_CHECKED})
target_link_libraries(test_xir_unit_locals_packet PRIVATE xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_xir_unit_locals_packet PRIVATE XR_CHECKED_FIXTURE="${XIR_UNIT_LOCALS_CHECKED}")
add_custom_command(OUTPUT ${XIR_UNIT_LOCALS_GENERATED}
    COMMAND $<TARGET_FILE:test_xir_unit_locals_packet> --write-c ${XIR_UNIT_LOCALS_CHECKED} ${XIR_UNIT_LOCALS_GENERATED}
    DEPENDS test_xir_unit_locals_packet ${XIR_UNIT_LOCALS_CHECKED}
    VERBATIM)
foreach(unit_locals_mode IN ITEMS native mixed)
    add_executable(test_xir_unit_locals_${unit_locals_mode} xir/test_xir_unit_locals_execution.c ${XIR_UNIT_LOCALS_GENERATED})
    target_link_libraries(test_xir_unit_locals_${unit_locals_mode} PRIVATE xray_xir_vm)
    if(unit_locals_mode STREQUAL "mixed")
        target_compile_definitions(test_xir_unit_locals_${unit_locals_mode} PRIVATE XR_UNIT_LOCALS_MIXED=1)
    else()
        target_compile_definitions(test_xir_unit_locals_${unit_locals_mode} PRIVATE XR_UNIT_LOCALS_MIXED=0)
    endif()
endforeach()
foreach(unit_locals_test IN ITEMS test_xir_unit_locals_source test_xir_unit_locals_packet test_xir_unit_locals_native test_xir_unit_locals_mixed)
    if(MSVC)
        target_compile_options(${unit_locals_test} PRIVATE /W4 /WX)
    else()
        target_compile_options(${unit_locals_test} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${unit_locals_test} COMMAND ${unit_locals_test})
    set_tests_properties(${unit_locals_test} PROPERTIES LABELS "unit;xir;execution;ownership;abi" TIMEOUT 300)
endforeach()
set_tests_properties(test_xir_unit_locals_native test_xir_unit_locals_mixed PROPERTIES TIMEOUT 600)
