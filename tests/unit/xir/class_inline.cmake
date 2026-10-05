# One source Checked packet and generated C feed VM/native/mixed class identity tests.
set(XIR_CLASS_INLINE_CHECKED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_class_inline.xrc)
set(XIR_CLASS_INLINE_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_class_inline.c)
add_executable(test_xir_class_inline_source xir/test_xir_class_inline_source.c)
target_sources(test_xir_class_inline_source PRIVATE xir/xir_class_inline_source_runtime.c)
target_link_libraries(test_xir_class_inline_source PRIVATE xray_xir_source xray_xir_vm)
target_compile_definitions(test_xir_class_inline_source PRIVATE
    XR_SOURCE_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_class_inline"
    XR_SOURCE_STDLIB="${CMAKE_SOURCE_DIR}/stdlib")
add_custom_command(OUTPUT ${XIR_CLASS_INLINE_CHECKED}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND $<TARGET_FILE:test_xir_class_inline_source> ${XIR_CLASS_INLINE_CHECKED}
    DEPENDS test_xir_class_inline_source
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_class_inline/root.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_class_inline/value.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_class_inline/reader.xr
    VERBATIM)
add_executable(test_xir_class_inline_packet xir/test_xir_class_inline_packet.c ${XIR_CLASS_INLINE_CHECKED})
target_link_libraries(test_xir_class_inline_packet PRIVATE xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_xir_class_inline_packet PRIVATE XR_CHECKED_FIXTURE="${XIR_CLASS_INLINE_CHECKED}")
add_custom_command(OUTPUT ${XIR_CLASS_INLINE_GENERATED}
    COMMAND $<TARGET_FILE:test_xir_class_inline_packet> ${XIR_CLASS_INLINE_GENERATED}
    DEPENDS test_xir_class_inline_packet ${XIR_CLASS_INLINE_CHECKED}
    VERBATIM)
foreach(class_inline_mode IN ITEMS native mixed)
    add_executable(test_xir_class_inline_${class_inline_mode} xir/test_xir_class_inline_execution.c ${XIR_CLASS_INLINE_GENERATED})
    target_link_libraries(test_xir_class_inline_${class_inline_mode} PRIVATE xray_xir_vm)
    if(class_inline_mode STREQUAL "mixed")
        target_compile_definitions(test_xir_class_inline_${class_inline_mode} PRIVATE XR_CLASS_INLINE_MIXED=1)
    else()
        target_compile_definitions(test_xir_class_inline_${class_inline_mode} PRIVATE XR_CLASS_INLINE_MIXED=0)
    endif()
endforeach()
foreach(class_inline_test IN ITEMS test_xir_class_inline_source test_xir_class_inline_packet test_xir_class_inline_native test_xir_class_inline_mixed)
    if(MSVC)
        target_compile_options(${class_inline_test} PRIVATE /W4 /WX)
    else()
        target_compile_options(${class_inline_test} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${class_inline_test} COMMAND ${class_inline_test})
    set_tests_properties(${class_inline_test} PROPERTIES LABELS "unit;xir;execution;ownership;abi")
endforeach()
