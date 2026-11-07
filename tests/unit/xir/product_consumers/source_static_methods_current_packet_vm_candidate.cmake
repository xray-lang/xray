# NOT_REGISTERED / NOT_RUN. Place beside the dedicated consumer driver and
# unchanged static shape/Source fixture. Both implementation observers and
# linked product libraries must resolve from one authorized immutable producer.
# Root shared material publication is NOT_PUBLISHED; this supplies no guard.
set(source_static_packet_candidate "${CMAKE_CURRENT_LIST_DIR}")
set(SOURCE_STATIC_CURRENT_FORMAL_ROOT "${CMAKE_SOURCE_DIR}" CACHE PATH
    "Authorized immutable producer tree owning the linked XIR targets and embedded implementations")
set(source_static_packet_driver "${source_static_packet_candidate}/test_source_static_methods_current_packet_vm.c")
set(source_static_packet_shape "${source_static_packet_candidate}/static_methods_source_shape.h")
set(source_static_packet_fixture "${source_static_packet_candidate}/fixtures/static_methods")
set(source_static_packet_material "${CMAKE_CURRENT_BINARY_DIR}/source-static-current-packet-only")
set(source_static_packet_file "${source_static_packet_material}/authentic-closed.chk")
set(source_static_packet_identity "${source_static_packet_material}/full-packet-identity.bin")
set(source_static_packet_writer test_source_static_methods_current_packet_writer)
set(source_static_packet_runner test_source_static_methods_current_packet_vm)
set(source_static_packet_outputs source_static_methods_current_packet_material)
foreach(source_static_packet_product IN ITEMS xray_xir_source_product xray_xir_vm)
    if(NOT TARGET ${source_static_packet_product})
        message(FATAL_ERROR "Candidate requires the authorized producer ${source_static_packet_product}")
    endif()
endforeach()
foreach(source_static_packet_helper IN ITEMS xir_source_program_compile_owner.h xir_runtime_allocations.h)
    if(NOT EXISTS "${SOURCE_STATIC_CURRENT_FORMAL_ROOT}/tests/unit/xir/${source_static_packet_helper}")
        message(FATAL_ERROR "Producer helper missing: ${source_static_packet_helper}")
    endif()
endforeach()

add_custom_target(${source_static_packet_outputs}
    DEPENDS "${source_static_packet_file}" "${source_static_packet_identity}")

# Keep the producer's PUBLIC native_cache/Source/scalar closure intact. The
# source-free claim concerns this process's request and API call path only.
