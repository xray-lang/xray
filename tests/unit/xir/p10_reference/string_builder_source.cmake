include_guard(GLOBAL)


foreach(case IN ITEMS basic order)
    set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated_native/builder_${case}.c")

    foreach(backend IN ITEMS native mixed)
        set(target test_xir_p10_reference_${backend}_builder_${case})

    endforeach()
endforeach()
