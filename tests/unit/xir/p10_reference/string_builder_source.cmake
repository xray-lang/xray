include_guard(GLOBAL)

foreach(case IN ITEMS basic rune i64 f64 bool null alias_clear order repeated_owner comptime concat_order)
    add_test(NAME test_xir_p10_string_builder_source_${case}
        COMMAND test_xir_p10_reference --accept builder_${case} 42)
    set_tests_properties(test_xir_p10_string_builder_source_${case} PROPERTIES
        TIMEOUT 30 LABELS "unit;xir;p10;string-builder;source")
endforeach()
foreach(case IN ITEMS basic order)
    set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated_native/builder_${case}.c")
    add_custom_command(OUTPUT "${generated}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated_native"
        COMMAND $<TARGET_FILE:test_xir_p10_reference> --emit builder_${case} 42 "${generated}"
        DEPENDS test_xir_p10_reference "${CMAKE_CURRENT_LIST_DIR}/fixtures/builder_${case}.xr"
        VERBATIM)
    foreach(backend IN ITEMS native mixed)
        set(target test_xir_p10_reference_${backend}_builder_${case})

    endforeach()
endforeach()
