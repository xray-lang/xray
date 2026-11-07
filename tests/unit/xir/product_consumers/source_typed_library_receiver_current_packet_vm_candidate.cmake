# Build and consume the same source tree's authentic typed Library packet.
# The writer preserves full Source verification and the receiver checks
# complete packet proof, producer death, execution and ownership.
set(source_typed_packet_formal_root "${CMAKE_SOURCE_DIR}")
set(source_typed_packet_driver "${CMAKE_CURRENT_LIST_DIR}/test_source_typed_library_receiver_current_packet_vm.c")
set(source_typed_packet_fixture
    "${source_typed_packet_formal_root}/tests/unit/xir/product_consumers/fixtures/module_receiver_suspend")
set(source_typed_packet_root "${CMAKE_BINARY_DIR}/generated/current-typed-library-packet-receiver-r1")
set(source_typed_packet_private "${source_typed_packet_root}/private-source")
set(source_typed_packet_material "${source_typed_packet_root}/packet")
set(source_typed_packet_file "${source_typed_packet_material}/authentic-closed.chk")
set(source_typed_packet_identity "${source_typed_packet_material}/full-packet-identity.bin")
set(source_typed_packet_writer test_source_typed_library_receiver_current_packet_writer)
set(source_typed_packet_runner test_source_typed_library_receiver_current_packet_vm)
set(source_typed_packet_outputs source_typed_library_receiver_current_packet_material)
foreach(source_typed_packet_product IN ITEMS xray_xir_source_product xray_xir_vm)
    if(NOT TARGET ${source_typed_packet_product})
        message(FATAL_ERROR "Typed packet receiver requires product target ${source_typed_packet_product}")
    endif()
endforeach()
foreach(source_typed_packet_helper IN ITEMS xir_source_program_compile_owner.h xir_runtime_allocations.h)
    if(NOT EXISTS "${source_typed_packet_formal_root}/tests/unit/xir/${source_typed_packet_helper}")
        message(FATAL_ERROR "Producer helper missing: ${source_typed_packet_helper}")
    endif()
endforeach()

add_custom_target(${source_typed_packet_outputs}
    DEPENDS "${source_typed_packet_file}" "${source_typed_packet_identity}")

# The producer's PUBLIC native-cache closure can still include Source. This
# receiver's request path is packet-only; physical SourceDelete remains OPEN.
