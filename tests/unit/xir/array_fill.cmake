

foreach(mode RANGE 0 0)
    set(executable test_xir_array_fill_faults)

    if(ENABLE_ASAN OR ENABLE_SANITIZERS)
        if(mode LESS 2)
            set(fill_compiler_cost 94)
        else()
            set(fill_compiler_cost 168)
        endif()
    else()
        set(fill_compiler_cost 21)
    endif()

endforeach()
