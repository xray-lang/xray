# Generate the authentic Source inputs once; rejected inputs emit no C.
set(CLASS_ACCESS_NATIVE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/class_access_native")
set(CLASS_ACCESS_NATIVE_C)
foreach(fixture IN ITEMS 0 1 3 5)
    list(APPEND CLASS_ACCESS_NATIVE_C "${CLASS_ACCESS_NATIVE_DIR}/class_access_${fixture}.c")
endforeach()
set(CLASS_ACCESS_NATIVE_H "${CLASS_ACCESS_NATIVE_DIR}/class_access_generated.h")
set(CLASS_ACCESS_NATIVE_INPUTS)
foreach(fixture IN ITEMS worker_return_formal worker_array_cow worker_carrier_unknown same_owner worker_module_alias worker_ref_array)
    list(APPEND CLASS_ACCESS_NATIVE_INPUTS "${CMAKE_SOURCE_DIR}/tests/fixtures/xir_cell_owner/class_access_${fixture}.xr")
endforeach()

foreach(backend IN ITEMS native mixed)
    set(target test_xir_class_access_${backend})

    foreach(mode IN ITEMS normal runtime-fi runtime-axes runtime-prepare runtime-control)
        string(REPLACE "-" "_" suffix "${mode}")

    endforeach()
endforeach()
