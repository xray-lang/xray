

file(MAKE_DIRECTORY "${H1_NATIVE_DIR}/resource_source")

foreach(_h1_kind native mixed)
    set(_h1_target "test_xir_root_parameter_${_h1_kind}_resources")

endforeach()

foreach(_h1_source apply fixed root forward)

    foreach(_h1_mode native native-vm vm-native)
        if(_h1_mode STREQUAL "native")
            set(_h1_target test_xir_root_parameter_native_resources)
        else()
            set(_h1_target test_xir_root_parameter_mixed_resources)
        endif()
        string(REPLACE "-" "_" _h1_name "${_h1_mode}")

    endforeach()
endforeach()
# Root registers strict external-oracle axes and every actual OOM range after
# auditing fresh producer and consumer censuses for each compiler and mode.
