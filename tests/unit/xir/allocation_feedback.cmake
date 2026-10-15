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
add_test(NAME test_xir_allocation_bookkeeping COMMAND test_xir_allocations --bookkeeping)
set_tests_properties(test_xir_allocation_bookkeeping PROPERTIES
    LABELS "unit;xir;memory;ownership;feedback")

# Scheduling estimates come from the current Windows Release and Debug ASan/UBSan runs.
# They do not change the aggregate runner deadline or any resource limit.
if(ENABLE_ASAN OR ENABLE_UBSAN)
    set_tests_properties(test_xir_allocations PROPERTIES COST 2179)
    set_tests_properties(test_xir_allocations_producer PROPERTIES COST 1892)
    set_tests_properties(test_xir_allocations_resources PROPERTIES COST 57)
    set_tests_properties(test_xir_allocations_semantics PROPERTIES COST 6)
    set_tests_properties(test_xir_allocations_runtime PROPERTIES COST 8)
else()
    set_tests_properties(test_xir_allocations PROPERTIES COST 235)
    set_tests_properties(test_xir_allocations_producer PROPERTIES COST 238)
    set_tests_properties(test_xir_allocations_resources PROPERTIES COST 5)
    set_tests_properties(test_xir_allocations_semantics test_xir_allocations_runtime PROPERTIES COST 1)
endif()
set_tests_properties(test_xir_allocations_first_case PROPERTIES COST 1)
