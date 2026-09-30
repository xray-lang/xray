# One frozen Checked packet feeds VM and the same generated-C native/mixed program.
set(XIR_DEPENDENCY_CHECKED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_dependency_ready.xrc)
set(XIR_DEPENDENCY_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_dependency_ready.c)
add_xray_bootstrap_executable(test_xir_dependency_source xir/test_xir_dependency_source.c)
target_link_libraries(test_xir_dependency_source PRIVATE xray_xir_source xray_xir_vm)
target_compile_definitions(test_xir_dependency_source PRIVATE
    XR_SOURCE_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_dependency_ready"
    XR_SOURCE_STDLIB="${CMAKE_SOURCE_DIR}/stdlib")
add_custom_command(OUTPUT ${XIR_DEPENDENCY_CHECKED}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND $<TARGET_FILE:test_xir_dependency_source> ${XIR_DEPENDENCY_CHECKED}
    DEPENDS test_xir_dependency_source
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_dependency_ready/root.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_dependency_ready/value.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_dependency_ready/reader.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_dependency_ready/negative_narrow.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_dependency_ready/negative_array.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_dependency_ready/negative_proof.xr
    VERBATIM)
add_executable(test_xir_dependency_packet xir/test_xir_dependency_packet.c ${XIR_DEPENDENCY_CHECKED})
target_link_libraries(test_xir_dependency_packet PRIVATE xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_xir_dependency_packet PRIVATE XR_CHECKED_FIXTURE="${XIR_DEPENDENCY_CHECKED}")
add_custom_command(OUTPUT ${XIR_DEPENDENCY_GENERATED}
    COMMAND $<TARGET_FILE:test_xir_dependency_packet> ${XIR_DEPENDENCY_GENERATED}
    DEPENDS test_xir_dependency_packet ${XIR_DEPENDENCY_CHECKED}
    VERBATIM)
foreach(dependency_mode IN ITEMS native mixed)
    add_executable(test_xir_dependency_${dependency_mode} xir/test_xir_dependency_execution.c ${XIR_DEPENDENCY_GENERATED})
    target_link_libraries(test_xir_dependency_${dependency_mode} PRIVATE xray_xir_vm)
    if(dependency_mode STREQUAL "mixed")
        target_compile_definitions(test_xir_dependency_${dependency_mode} PRIVATE XR_DEPENDENCY_MIXED=1)
    else()
        target_compile_definitions(test_xir_dependency_${dependency_mode} PRIVATE XR_DEPENDENCY_MIXED=0)
    endif()
endforeach()
foreach(dependency_test IN ITEMS test_xir_dependency_source test_xir_dependency_packet test_xir_dependency_native test_xir_dependency_mixed)
    if(MSVC)
        target_compile_options(${dependency_test} PRIVATE /W4 /WX)
    else()
        target_compile_options(${dependency_test} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${dependency_test} COMMAND ${dependency_test})
    set_tests_properties(${dependency_test} PROPERTIES LABELS "unit;xir;execution;ownership;abi" TIMEOUT 300)
endforeach()
set_tests_properties(test_xir_dependency_native test_xir_dependency_mixed PROPERTIES TIMEOUT 600)
