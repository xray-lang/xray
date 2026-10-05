# Direct source-owned Checked executes after producer destruction.
# Packet VM and generated native/mixed consumers link no source, parser or AST.
# One frozen Checked packet feeds those three independent source-free executions.
set(XIR_INITIALIZATION_CHECKED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_initialization_failure.xrc)
set(XIR_INITIALIZATION_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_initialization_failure.c)
add_executable(test_xir_initialization_failure_source xir/test_xir_initialization_failure_source.c)
target_sources(test_xir_initialization_failure_source PRIVATE xir/xir_initialization_failure_source_runtime.c)
target_link_libraries(test_xir_initialization_failure_source PRIVATE xray_xir_source xray_xir_vm)
target_compile_definitions(test_xir_initialization_failure_source PRIVATE
    XR_SOURCE_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_initialization_failure"
    XR_SOURCE_STDLIB="${CMAKE_SOURCE_DIR}/stdlib")
add_custom_command(OUTPUT ${XIR_INITIALIZATION_CHECKED}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND $<TARGET_FILE:test_xir_initialization_failure_source> ${XIR_INITIALIZATION_CHECKED}
    DEPENDS test_xir_initialization_failure_source
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_initialization_failure/root.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_initialization_failure/base.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_initialization_failure/failing.xr
    VERBATIM)
add_executable(test_xir_initialization_failure_packet xir/test_xir_initialization_failure_packet.c ${XIR_INITIALIZATION_CHECKED})
target_link_libraries(test_xir_initialization_failure_packet PRIVATE xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_xir_initialization_failure_packet PRIVATE XR_CHECKED_FIXTURE="${XIR_INITIALIZATION_CHECKED}")
add_custom_command(OUTPUT ${XIR_INITIALIZATION_GENERATED}
    COMMAND $<TARGET_FILE:test_xir_initialization_failure_packet> ${XIR_INITIALIZATION_GENERATED}
    DEPENDS test_xir_initialization_failure_packet ${XIR_INITIALIZATION_CHECKED}
    VERBATIM)
foreach(initialization_failure_mode IN ITEMS native mixed)
    add_executable(test_xir_initialization_failure_${initialization_failure_mode} xir/test_xir_initialization_failure_execution.c ${XIR_INITIALIZATION_GENERATED})
    target_link_libraries(test_xir_initialization_failure_${initialization_failure_mode} PRIVATE xray_xir_vm)
    if(initialization_failure_mode STREQUAL "mixed")
        target_compile_definitions(test_xir_initialization_failure_${initialization_failure_mode} PRIVATE XR_INITIALIZATION_MIXED=1)
    else()
        target_compile_definitions(test_xir_initialization_failure_${initialization_failure_mode} PRIVATE XR_INITIALIZATION_MIXED=0)
    endif()
endforeach()
add_executable(test_xir_initialization_publication xir/test_xir_initialization_publication.c)
target_link_libraries(test_xir_initialization_publication PRIVATE xray_xir_vm xray_xir_cgen)
foreach(initialization_failure_test IN ITEMS test_xir_initialization_publication test_xir_initialization_failure_source test_xir_initialization_failure_packet test_xir_initialization_failure_native test_xir_initialization_failure_mixed)
    if(MSVC)
        target_compile_options(${initialization_failure_test} PRIVATE /W4 /WX)
    else()
        target_compile_options(${initialization_failure_test} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${initialization_failure_test} COMMAND ${initialization_failure_test})
    set_tests_properties(${initialization_failure_test} PROPERTIES LABELS "unit;xir;execution;ownership;abi" TIMEOUT 300)
endforeach()
set_tests_properties(test_xir_initialization_failure_native test_xir_initialization_failure_mixed PROPERTIES TIMEOUT 600)
