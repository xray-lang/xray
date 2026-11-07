# Build the authentic typed Library writer and receivers from this source tree.
# Ordinary and cancellation registrations have distinct qualification scopes.
set(SOURCE_TYPED_LIBRARY_CURRENT_FORMAL_ROOT "${CMAKE_SOURCE_DIR}")
set(source_typed_native_candidate "${CMAKE_CURRENT_LIST_DIR}")
set(source_typed_native_driver "${source_typed_native_candidate}/test_source_typed_library_receiver_current_native_mixed.c")
set(source_typed_native_support "${source_typed_native_candidate}/typed_library_native_mixed_support.h")
set(source_typed_native_fixture
    "${SOURCE_TYPED_LIBRARY_CURRENT_FORMAL_ROOT}/tests/unit/xir/product_consumers/fixtures/module_receiver_suspend")
set(source_typed_native_generated "${CMAKE_BINARY_DIR}/generated/current-typed-library-native-mixed-r1")
set(source_typed_native_private "${source_typed_native_generated}/private-source")
set(source_typed_native_material "${source_typed_native_generated}/material")
set(source_typed_native_writer test_source_typed_library_receiver_current_native_writer)
set(source_typed_native_runner test_source_typed_library_receiver_current_native_mixed)
foreach(source_typed_native_product IN ITEMS xray_xir_source_product xray_xir_vm)
    if(NOT TARGET ${source_typed_native_product})
        message(FATAL_ERROR "Candidate requires immutable provider ${source_typed_native_product}")
    endif()
endforeach()
foreach(source_typed_native_helper IN ITEMS xir_source_program_compile_owner.h xir_runtime_allocations.h)
    if(NOT EXISTS "${SOURCE_TYPED_LIBRARY_CURRENT_FORMAL_ROOT}/tests/unit/xir/${source_typed_native_helper}")
        message(FATAL_ERROR "Provider helper missing: ${source_typed_native_helper}")
    endif()
endforeach()
foreach(source_typed_native_leaf IN ITEMS root.xr library.xr)
    if(NOT EXISTS "${source_typed_native_fixture}/${source_typed_native_leaf}")
        message(FATAL_ERROR "Complete original typed Library Source input missing: ${source_typed_native_leaf}")
    endif()
endforeach()

set(source_typed_native_c "${source_typed_native_material}/actual_payload.c")
set(source_typed_native_proof "${source_typed_native_material}/actual_proof.bin")
set(source_typed_native_identity "${source_typed_native_material}/actual_identity.bin")
set(source_typed_native_binding "${source_typed_native_material}/payload_binding.c")

# Cancellation reuses the same four authentic outputs and real host C/binding inputs.
set(source_typed_native_cancel_runner test_source_typed_library_receiver_current_native_mixed_cancel_prefix)

# The public VM native-cache closure still includes Source; physical SourceDelete is OPEN.

# The original three ordinary tests retain their frozen independent facet.
# This separate facet drives only one call at a time with two initialized live Instances.
