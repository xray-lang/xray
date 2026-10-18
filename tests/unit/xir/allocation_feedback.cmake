# The native no-argument executable remains the complete serial reference.
# Parent gates own the same complete case union within one global deadline.
foreach(allocation_phase semantics runtime resources)
    add_test(NAME test_xir_allocations_${allocation_phase}
        COMMAND test_xir_allocations --${allocation_phase})
    set_tests_properties(test_xir_allocations_${allocation_phase} PROPERTIES
        LABELS "unit;xir;memory;ownership;feedback")
endforeach()
set(allocation_case_driver ${CMAKE_CURRENT_SOURCE_DIR}/xir/run_allocation_case_faults.py)
set(allocation_case_arguments --executable $<TARGET_FILE:test_xir_allocations>
    --source-root ${CMAKE_SOURCE_DIR} --build-root ${CMAKE_BINARY_DIR})
add_test(NAME test_xir_allocations_producer
    COMMAND ${Python3_EXECUTABLE} ${allocation_case_driver} ${allocation_case_arguments} --mode producer)
set_tests_properties(test_xir_allocations_producer PROPERTIES
    LABELS "unit;xir;memory;ownership;feedback")
add_test(NAME test_xir_allocations_partition_semantics
    COMMAND ${Python3_EXECUTABLE} ${allocation_case_driver} ${allocation_case_arguments} --mode normal)
set_tests_properties(test_xir_allocations test_xir_allocations_producer test_xir_allocations_partition_semantics
    PROPERTIES TIMEOUT 300 PROCESSORS 12 RUN_SERIAL TRUE)
set_tests_properties(test_xir_allocations_partition_semantics PROPERTIES
    LABELS "unit;xir;memory;ownership;feedback;test-runner" COST 15)
add_test(NAME test_xir_allocation_case_driver
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/xir/test_allocation_case_driver.py
        ${allocation_case_arguments})
set_tests_properties(test_xir_allocation_case_driver PROPERTIES
    LABELS "unit;xir;ownership;test-runner;windows" TIMEOUT 120 PROCESSORS 12
    RUN_SERIAL TRUE SKIP_RETURN_CODE 77 COST 15)
add_test(NAME test_xir_allocations_first_case COMMAND test_xir_allocations --first-case)
set_tests_properties(test_xir_allocations_first_case PROPERTIES
    LABELS "unit;xir;memory;ownership;feedback;equivalence")
add_test(NAME test_xir_allocation_bookkeeping COMMAND test_xir_allocations --bookkeeping)
set_tests_properties(test_xir_allocation_bookkeeping PROPERTIES
    LABELS "unit;xir;memory;ownership;feedback")

# Historical serial costs remain estimates until complete parent runs are measured.
# The parent deadline is the original ordinary sanitizer timeout, never per child.
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
