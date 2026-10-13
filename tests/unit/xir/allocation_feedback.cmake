# Each entry runs the real receiver and an independently measured producer.
# The no-argument aggregate retains all original resource and fault cases.
foreach(allocation_phase semantics producer runtime resources)
    add_test(NAME test_xir_allocations_${allocation_phase}
        COMMAND test_xir_allocations --${allocation_phase})
    set_tests_properties(test_xir_allocations_${allocation_phase} PROPERTIES
        LABELS "unit;xir;memory;ownership;feedback")
endforeach()
add_test(NAME test_xir_allocations_first_case COMMAND test_xir_allocations --first-case)
set_tests_properties(test_xir_allocations_first_case PROPERTIES
    LABELS "unit;xir;memory;ownership;feedback;equivalence")
