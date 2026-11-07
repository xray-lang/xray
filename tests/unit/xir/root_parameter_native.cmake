# The producer owns Source, Checked replay, specialization and canonical CGen.
set(H1_NATIVE_DIR "${CMAKE_BINARY_DIR}/generated/root_parameter_native")
set(H1_NATIVE_C)
foreach(_h1_index RANGE 0 3)
    list(APPEND H1_NATIVE_C "${H1_NATIVE_DIR}/case${_h1_index}.c")
endforeach()
set(H1_NATIVE_H "${H1_NATIVE_DIR}/root_parameter_native_generated.h")

foreach(_h1_kind native mixed)
    set(_h1_target "test_xir_root_parameter_${_h1_kind}")

endforeach()

foreach(_h1_source apply fixed root forward)
    foreach(_h1_mode native native-vm vm-native)
        if(_h1_mode STREQUAL "native")
            set(_h1_target test_xir_root_parameter_native)
        else()
            set(_h1_target test_xir_root_parameter_mixed)
        endif()
        string(REPLACE "-" "_" _h1_test_mode "${_h1_mode}")
        set(_h1_test "test_xir_root_parameter_${_h1_test_mode}_${_h1_source}")

    endforeach()
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/root_parameter_native_resources.cmake")
