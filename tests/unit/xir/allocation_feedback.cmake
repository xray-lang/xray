# The native no-argument executable remains the complete serial reference.
# Parent gates own the same complete case union within one global deadline.
set(allocation_case_driver ${CMAKE_CURRENT_SOURCE_DIR}/xir/run_allocation_case_faults.py)
set(allocation_case_arguments --executable $<TARGET_FILE:test_xir_allocations>
    --source-root ${CMAKE_SOURCE_DIR} --build-root ${CMAKE_BINARY_DIR})

# Historical serial costs remain estimates until complete parent runs are measured.
# The parent deadline is the original ordinary sanitizer timeout, never per child.
