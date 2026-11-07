# Nine shared typed Source programs produce canonical C and owned Programs.
set(TYPED_NATIVE_DIR "${CMAKE_BINARY_DIR}/generated/library_typed_native")
set(TYPED_NATIVE_C)
foreach(_typed_index RANGE 0 8)
    list(APPEND TYPED_NATIVE_C "${TYPED_NATIVE_DIR}/case${_typed_index}.c")
endforeach()
set(TYPED_NATIVE_H "${TYPED_NATIVE_DIR}/library_typed_native_generated.h")

foreach(_typed_kind native mixed)
    set(_typed_target "test_xir_library_typed_${_typed_kind}")

endforeach()

foreach(_typed_case typed_nominal typed_interface construction generic_parent_lr generic_parent_rl member_witness carrier_member enum_owned class_owned)

    foreach(_typed_mode native vm native-vm vm-native)
        if(_typed_mode STREQUAL "native")
            set(_typed_target test_xir_library_typed_native)
        else()
            set(_typed_target test_xir_library_typed_mixed)
        endif()
        string(REPLACE "-" "_" _typed_test_mode "${_typed_mode}")
        set(_typed_test "test_xir_library_typed_${_typed_test_mode}_${_typed_case}")

    endforeach()
endforeach()
