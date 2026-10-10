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
add_executable(${source_static_packet_writer} "${source_static_packet_driver}")
target_compile_definitions(${source_static_packet_writer} PRIVATE XR_SOURCE_STATIC_PACKET_WRITER=1)
target_link_libraries(${source_static_packet_writer} PRIVATE xray_xir_source_product)
add_custom_command(
    OUTPUT "${source_static_packet_file}" "${source_static_packet_identity}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${source_static_packet_material}"
    COMMAND $<TARGET_FILE:${source_static_packet_writer}>
        "${source_static_packet_fixture}" "${source_static_packet_fixture}/root.xr"
        "${SOURCE_STATIC_CURRENT_FORMAL_ROOT}/stdlib"
        "${source_static_packet_file}" "${source_static_packet_identity}"
    DEPENDS ${source_static_packet_writer} "${source_static_packet_driver}"
        "${source_static_packet_shape}" "${source_static_packet_fixture}/root.xr"
    COMMENT "Write authentic CLOSED bytes and matching full packet identity, normal path only"
    VERBATIM)
add_custom_target(${source_static_packet_outputs}
    DEPENDS "${source_static_packet_file}" "${source_static_packet_identity}")
add_executable(${source_static_packet_runner} "${source_static_packet_driver}")
target_link_libraries(${source_static_packet_runner} PRIVATE xray_xir_vm)
# Keep the producer's PUBLIC native_cache/Source/scalar closure intact. The
# source-free claim concerns this process's request and API call path only.
add_dependencies(${source_static_packet_runner} ${source_static_packet_outputs})
foreach(source_static_packet_target IN ITEMS ${source_static_packet_writer} ${source_static_packet_runner})
    target_include_directories(${source_static_packet_target} BEFORE PRIVATE
        "${SOURCE_STATIC_CURRENT_FORMAL_ROOT}/tests/unit/xir"
        "${SOURCE_STATIC_CURRENT_FORMAL_ROOT}/src" "${source_static_packet_candidate}")
    set_target_properties(${source_static_packet_target} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_definitions(${source_static_packet_target} PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(${source_static_packet_target} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${source_static_packet_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME ${source_static_packet_runner} COMMAND ${source_static_packet_runner}
    "${source_static_packet_file}" "${source_static_packet_identity}")
set_tests_properties(${source_static_packet_runner} PROPERTIES TIMEOUT 120 PROCESSORS 1
    WORKING_DIRECTORY "${source_static_packet_material}"
    LABELS "unit;xir;program-consumer;class;static-method;ownership;packet;source-free;vm;normal-only;current-interface-candidate")
