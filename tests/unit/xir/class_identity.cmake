# One source Checked packet and generated C feed VM/native/mixed class identity tests.
set(XIR_CLASS_CHECKED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_class_identity.xrc)
set(XIR_CLASS_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_class_identity.c)
add_xray_bootstrap_executable(test_xir_class_source xir/test_xir_class_source.c)
target_link_libraries(test_xir_class_source PRIVATE xray_xir_source xray_xir_vm)
target_compile_definitions(test_xir_class_source PRIVATE
    XR_SOURCE_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_class_identity"
    XR_SOURCE_STDLIB="${CMAKE_SOURCE_DIR}/stdlib")
add_custom_command(OUTPUT ${XIR_CLASS_CHECKED}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND $<TARGET_FILE:test_xir_class_source> ${XIR_CLASS_CHECKED}
    DEPENDS test_xir_class_source
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_class_identity/root.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_class_identity/value.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_class_identity/reader.xr
    VERBATIM)
add_executable(test_xir_class_packet xir/test_xir_class_packet.c ${XIR_CLASS_CHECKED})
target_link_libraries(test_xir_class_packet PRIVATE xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_xir_class_packet PRIVATE XR_CHECKED_FIXTURE="${XIR_CLASS_CHECKED}")
add_custom_command(OUTPUT ${XIR_CLASS_GENERATED}
    COMMAND $<TARGET_FILE:test_xir_class_packet> ${XIR_CLASS_GENERATED}
    DEPENDS test_xir_class_packet ${XIR_CLASS_CHECKED}
    VERBATIM)
foreach(class_mode IN ITEMS native mixed)
    add_executable(test_xir_class_${class_mode} xir/test_xir_class_execution.c ${XIR_CLASS_GENERATED})
    target_link_libraries(test_xir_class_${class_mode} PRIVATE xray_xir_vm)
    if(class_mode STREQUAL "mixed")
        target_compile_definitions(test_xir_class_${class_mode} PRIVATE XR_CLASS_MIXED=1)
    else()
        target_compile_definitions(test_xir_class_${class_mode} PRIVATE XR_CLASS_MIXED=0)
    endif()
endforeach()
foreach(class_test IN ITEMS test_xir_class_source test_xir_class_packet test_xir_class_native test_xir_class_mixed)
    if(MSVC)
        target_compile_options(${class_test} PRIVATE /W4 /WX)
    else()
        target_compile_options(${class_test} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${class_test} COMMAND ${class_test})
    set_tests_properties(${class_test} PROPERTIES LABELS "unit;xir;execution;ownership;abi")
endforeach()
