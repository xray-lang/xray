# STATIC_NOT_RUN / NOT_REGISTERED. Place beside the dedicated packet driver.
# Public interface reference: Checked 27/72, Library C interface 2,
# Value 22 / Call 28 / Program 29. No producer authorization is supplied here.
# Qualification requires independently sealed same-provider source, helpers,
# libraries, stdlib, tools and generated packet bytes; path checks are not a guard.
set(SOURCE_TYPED_LIBRARY_CURRENT_FORMAL_ROOT "${CMAKE_SOURCE_DIR}" CACHE PATH
    "Authorized immutable producer tree owning product targets, embedded helpers and full Source inputs")
set(source_typed_packet_driver "${CMAKE_CURRENT_LIST_DIR}/test_source_typed_library_receiver_current_packet_vm.c")
set(source_typed_packet_fixture
    "${SOURCE_TYPED_LIBRARY_CURRENT_FORMAL_ROOT}/tests/unit/xir/product_consumers/fixtures/module_receiver_suspend")
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
        message(FATAL_ERROR "Candidate requires the authorized producer ${source_typed_packet_product}")
    endif()
endforeach()
foreach(source_typed_packet_helper IN ITEMS xir_source_program_compile_owner.h xir_runtime_allocations.h)
    if(NOT EXISTS "${SOURCE_TYPED_LIBRARY_CURRENT_FORMAL_ROOT}/tests/unit/xir/${source_typed_packet_helper}")
        message(FATAL_ERROR "Producer helper missing: ${source_typed_packet_helper}")
    endif()
endforeach()
add_executable(${source_typed_packet_writer} "${source_typed_packet_driver}")
target_compile_definitions(${source_typed_packet_writer} PRIVATE XR_SOURCE_TYPED_LIBRARY_PACKET_WRITER=1)
target_link_libraries(${source_typed_packet_writer} PRIVATE xray_xir_source_product)
add_custom_command(
    OUTPUT "${source_typed_packet_file}" "${source_typed_packet_identity}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${source_typed_packet_private}" "${source_typed_packet_material}"
    COMMAND $<TARGET_FILE:${source_typed_packet_writer}>
        "${source_typed_packet_private}" "${source_typed_packet_fixture}"
        "${SOURCE_TYPED_LIBRARY_CURRENT_FORMAL_ROOT}/stdlib"
        "${source_typed_packet_file}" "${source_typed_packet_identity}"
    DEPENDS ${source_typed_packet_writer} "${source_typed_packet_driver}"
        "${source_typed_packet_fixture}/root.xr" "${source_typed_packet_fixture}/library.xr"
        "${SOURCE_TYPED_LIBRARY_CURRENT_FORMAL_ROOT}/stdlib/types/coro.xr"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "Write authentic typed Library CLOSED packet and full identity without runtime calls"
    VERBATIM)
add_custom_target(${source_typed_packet_outputs}
    DEPENDS "${source_typed_packet_file}" "${source_typed_packet_identity}")
add_executable(${source_typed_packet_runner} "${source_typed_packet_driver}")
target_link_libraries(${source_typed_packet_runner} PRIVATE xray_xir_vm)
# The producer's PUBLIC native-cache closure can still include Source. This
# receiver's request path is packet-only; physical SourceDelete remains OPEN.
add_dependencies(${source_typed_packet_runner} ${source_typed_packet_outputs})
foreach(source_typed_packet_target IN ITEMS ${source_typed_packet_writer} ${source_typed_packet_runner})
    target_include_directories(${source_typed_packet_target} BEFORE PRIVATE
        "${SOURCE_TYPED_LIBRARY_CURRENT_FORMAL_ROOT}/tests/unit/xir"
        "${SOURCE_TYPED_LIBRARY_CURRENT_FORMAL_ROOT}/src" "${CMAKE_CURRENT_LIST_DIR}")
    set_target_properties(${source_typed_packet_target} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_definitions(${source_typed_packet_target} PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(${source_typed_packet_target} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${source_typed_packet_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME ${source_typed_packet_runner} COMMAND ${source_typed_packet_runner}
    "${source_typed_packet_file}" "${source_typed_packet_identity}")
set_tests_properties(${source_typed_packet_runner} PROPERTIES
    WORKING_DIRECTORY "${source_typed_packet_material}" TIMEOUT 120 PROCESSORS 1 RUN_SERIAL TRUE
    LABELS "unit;xir;program-consumer;typed-library;module-graph;class;receiver;coroutine;ownership;packet;source-free;vm;normal-only;current-interface-candidate")
