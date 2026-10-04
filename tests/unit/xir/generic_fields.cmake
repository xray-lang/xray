# Direct source-owned Checked executes after producer destruction.
# Packet VM and generated native/mixed consumers link no source, parser or AST.
# One frozen Checked packet feeds those three independent source-free executions.
set(XIR_GENERIC_FIELDS_CHECKED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_generic_fields.xrc)
set(XIR_GENERIC_FIELDS_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_generic_fields.c)
add_executable(test_xir_generic_fields_source xir/test_xir_generic_fields_source.c)
target_sources(test_xir_generic_fields_source PRIVATE xir/xir_generic_fields_source_runtime.c)
target_link_libraries(test_xir_generic_fields_source PRIVATE xray_xir_source xray_xir_vm)
target_compile_definitions(test_xir_generic_fields_source PRIVATE
    XR_SOURCE_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_generic_fields"
    XR_SOURCE_STDLIB="${CMAKE_SOURCE_DIR}/stdlib")
add_custom_command(OUTPUT ${XIR_GENERIC_FIELDS_CHECKED}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND $<TARGET_FILE:test_xir_generic_fields_source> ${XIR_GENERIC_FIELDS_CHECKED}
    DEPENDS test_xir_generic_fields_source
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_generic_fields/root.xr
    VERBATIM)
add_executable(test_xir_generic_fields_packet xir/test_xir_generic_fields_packet.c ${XIR_GENERIC_FIELDS_CHECKED})
target_link_libraries(test_xir_generic_fields_packet PRIVATE xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_xir_generic_fields_packet PRIVATE XR_CHECKED_FIXTURE="${XIR_GENERIC_FIELDS_CHECKED}")
add_custom_command(OUTPUT ${XIR_GENERIC_FIELDS_GENERATED}
    COMMAND $<TARGET_FILE:test_xir_generic_fields_packet> ${XIR_GENERIC_FIELDS_GENERATED}
    DEPENDS test_xir_generic_fields_packet ${XIR_GENERIC_FIELDS_CHECKED}
    VERBATIM)
foreach(generic_fields_mode IN ITEMS native mixed)
    add_executable(test_xir_generic_fields_${generic_fields_mode} xir/test_xir_generic_fields_execution.c ${XIR_GENERIC_FIELDS_GENERATED})
    target_link_libraries(test_xir_generic_fields_${generic_fields_mode} PRIVATE xray_xir_vm)
    if(generic_fields_mode STREQUAL "mixed")
        target_compile_definitions(test_xir_generic_fields_${generic_fields_mode} PRIVATE XR_GENERIC_FIELDS_MIXED=1)
    else()
        target_compile_definitions(test_xir_generic_fields_${generic_fields_mode} PRIVATE XR_GENERIC_FIELDS_MIXED=0)
    endif()
endforeach()
foreach(generic_fields_test IN ITEMS test_xir_generic_fields_source test_xir_generic_fields_packet test_xir_generic_fields_native test_xir_generic_fields_mixed)
    if(MSVC)
        target_compile_options(${generic_fields_test} PRIVATE /W4 /WX)
    else()
        target_compile_options(${generic_fields_test} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${generic_fields_test} COMMAND ${generic_fields_test})
    set_tests_properties(${generic_fields_test} PROPERTIES LABELS "unit;xir;execution;ownership;abi" TIMEOUT 300)
endforeach()
set_tests_properties(test_xir_generic_fields_native test_xir_generic_fields_mixed PROPERTIES TIMEOUT 600)
