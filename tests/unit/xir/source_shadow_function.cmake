set(SHADOW_C "${CMAKE_BINARY_DIR}/generated/xir_shadow_function.c")

set(SHADOW_REF_C "${CMAKE_BINARY_DIR}/generated/xir_shadow_ref.c")

set(shadow_modes vm native mixed_root mixed_import)
foreach(component shadow_function shadow_ref)
    foreach(mode RANGE 0 3)
        list(GET shadow_modes ${mode} suffix)
        if(mode EQUAL 0)
            set(binary test_xir_source_${component})
        else()
            set(binary test_xir_source_${component}_native)
        endif()
        if(WIN32)
            find_package(Python3 COMPONENTS Interpreter REQUIRED)

        else()

        endif()

    endforeach()
endforeach()
