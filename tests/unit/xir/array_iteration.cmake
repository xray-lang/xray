# One source Checked packet and generated C feed VM/native/mixed Array iteration tests.
set(XIR_ITERATION_CHECKED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_array_iteration.xrc)
set(XIR_ITERATION_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_array_iteration.c)
add_executable(test_xir_iteration_source xir/test_xir_iteration_source.c)
target_link_libraries(test_xir_iteration_source PRIVATE xray_xir_source xray_xir_vm)
target_compile_definitions(test_xir_iteration_source PRIVATE
    XR_SOURCE_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_array_iteration"
    XR_SOURCE_STDLIB="${CMAKE_SOURCE_DIR}/stdlib")
add_custom_command(OUTPUT ${XIR_ITERATION_CHECKED}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND $<TARGET_FILE:test_xir_iteration_source> ${XIR_ITERATION_CHECKED}
    DEPENDS test_xir_iteration_source
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_array_iteration/root.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_array_iteration/value.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_array_iteration/reader.xr
    VERBATIM)
add_executable(test_xir_iteration_packet xir/test_xir_iteration_packet.c ${XIR_ITERATION_CHECKED})
target_link_libraries(test_xir_iteration_packet PRIVATE xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_xir_iteration_packet PRIVATE XR_CHECKED_FIXTURE="${XIR_ITERATION_CHECKED}")
add_custom_command(OUTPUT ${XIR_ITERATION_GENERATED}
    COMMAND $<TARGET_FILE:test_xir_iteration_packet> ${XIR_ITERATION_GENERATED}
    DEPENDS test_xir_iteration_packet ${XIR_ITERATION_CHECKED}
    VERBATIM)
foreach(iteration_mode IN ITEMS native mixed)
    add_executable(test_xir_iteration_${iteration_mode} xir/test_xir_iteration_execution.c ${XIR_ITERATION_GENERATED})
    target_link_libraries(test_xir_iteration_${iteration_mode} PRIVATE xray_xir_vm)
    if(iteration_mode STREQUAL "mixed")
        target_compile_definitions(test_xir_iteration_${iteration_mode} PRIVATE XR_ITERATION_MIXED=1)
    else()
        target_compile_definitions(test_xir_iteration_${iteration_mode} PRIVATE XR_ITERATION_MIXED=0)
    endif()
endforeach()
foreach(iteration_test IN ITEMS test_xir_iteration_source test_xir_iteration_packet test_xir_iteration_native test_xir_iteration_mixed)
    if(MSVC)
        target_compile_options(${iteration_test} PRIVATE /W4 /WX)
    else()
        target_compile_options(${iteration_test} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${iteration_test} COMMAND ${iteration_test})
    set_tests_properties(${iteration_test} PROPERTIES LABELS "unit;xir;execution;ownership;abi")
endforeach()
