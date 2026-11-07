# Five real Source dependency graphs produce canonical C and owned Programs.
set(VIEWS_NATIVE_DIR "${CMAKE_BINARY_DIR}/generated/library_views_native")
set(VIEWS_NATIVE_C)
foreach(_views_index RANGE 0 4)
    list(APPEND VIEWS_NATIVE_C "${VIEWS_NATIVE_DIR}/case${_views_index}.c")
endforeach()
set(VIEWS_NATIVE_H "${VIEWS_NATIVE_DIR}/library_views_native_generated.h")

foreach(_views_kind native mixed)
    set(_views_target "test_xir_library_views_${_views_kind}")

endforeach()

foreach(_views_case diamond leaf disjoint reverse chain17)

    foreach(_views_mode native native-vm vm-native)
        if(_views_mode STREQUAL "native")
            set(_views_target test_xir_library_views_native)
        else()
            set(_views_target test_xir_library_views_mixed)
        endif()
        string(REPLACE "-" "_" _views_test_mode "${_views_mode}")
        set(_views_test "test_xir_library_views_${_views_test_mode}_${_views_case}")

    endforeach()
endforeach()
