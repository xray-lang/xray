

foreach(backend IN ITEMS 0 1 vm native)

    foreach(close RANGE 0 1)
        foreach(drop RANGE 0 1)
            set(name "test_xir_mixed_atomic_resources_axes_${backend}_${close}_${drop}")

        endforeach()
    endforeach()
endforeach()
include("${CMAKE_CURRENT_LIST_DIR}/atomic_runtime_fi_cases.cmake")
