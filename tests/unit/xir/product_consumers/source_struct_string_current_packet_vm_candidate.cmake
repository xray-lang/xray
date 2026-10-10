# STATIC_NOT_RUN / NOT_REGISTERED. Place beside the dedicated packet driver.
# Public Git reference 6828c1cb3bd33be9d97aa7050a95997cfb690d47:
# Checked 28/73, Value 23 / Call 29 / Program 30. This grants no producer or window.
# Qualification requires sealed same-provider source, embedded helpers, product
# libraries, stdlib, tools, consumer-owned shape/fixture and generated packet bytes.
# Path checks below are dependency checks, not an immutable material guard.
set(SOURCE_STRUCT_STRING_CURRENT_FORMAL_ROOT "${CMAKE_SOURCE_DIR}" CACHE PATH
    "Authorized immutable provider tree owning product targets, embedded helpers and stdlib")
set(source_struct_string_packet_driver "${CMAKE_CURRENT_LIST_DIR}/test_source_struct_string_current_packet_vm.c")
set(source_struct_string_packet_shape "${CMAKE_CURRENT_LIST_DIR}/struct_string_current_packet_shape.h")
set(source_struct_string_packet_fixture "${CMAKE_CURRENT_LIST_DIR}/fixtures/struct_string")
set(source_struct_string_packet_root "${CMAKE_BINARY_DIR}/generated/current-struct-string-packet-r1")
set(source_struct_string_packet_private "${source_struct_string_packet_root}/private-source")
set(source_struct_string_packet_material "${source_struct_string_packet_root}/packet")
set(source_struct_string_packet_file "${source_struct_string_packet_material}/authentic-closed.chk")
set(source_struct_string_packet_identity "${source_struct_string_packet_material}/full-packet-identity.bin")
set(source_struct_string_packet_writer test_source_struct_string_current_packet_writer)
set(source_struct_string_packet_runner test_source_struct_string_current_packet_vm)
set(source_struct_string_packet_outputs source_struct_string_current_packet_material)
foreach(source_struct_string_packet_product IN ITEMS xray_xir_source_product xray_xir_vm)
    if(NOT TARGET ${source_struct_string_packet_product})
        message(FATAL_ERROR "Candidate requires the authorized provider ${source_struct_string_packet_product}")
    endif()
endforeach()
foreach(source_struct_string_packet_helper IN ITEMS xir_source_program_compile_owner.h xir_runtime_allocations.h)
    if(NOT EXISTS "${SOURCE_STRUCT_STRING_CURRENT_FORMAL_ROOT}/tests/unit/xir/${source_struct_string_packet_helper}")
        message(FATAL_ERROR "Provider helper missing: ${source_struct_string_packet_helper}")
    endif()
endforeach()
if(NOT EXISTS "${source_struct_string_packet_shape}" OR NOT EXISTS "${source_struct_string_packet_fixture}/root.xr"
    OR NOT IS_DIRECTORY "${SOURCE_STRUCT_STRING_CURRENT_FORMAL_ROOT}/stdlib")
    message(FATAL_ERROR "Candidate requires its complete Struct/String shape/fixture and provider stdlib")
endif()
add_executable(${source_struct_string_packet_writer} "${source_struct_string_packet_driver}" "${source_struct_string_packet_shape}")
target_compile_definitions(${source_struct_string_packet_writer} PRIVATE XR_SOURCE_STRUCT_STRING_PACKET_WRITER=1)
target_link_libraries(${source_struct_string_packet_writer} PRIVATE xray_xir_source_product)
add_custom_command(
    OUTPUT "${source_struct_string_packet_file}" "${source_struct_string_packet_identity}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${source_struct_string_packet_private}" "${source_struct_string_packet_material}"
    COMMAND $<TARGET_FILE:${source_struct_string_packet_writer}>
        "${source_struct_string_packet_private}" "${source_struct_string_packet_fixture}"
        "${SOURCE_STRUCT_STRING_CURRENT_FORMAL_ROOT}/stdlib"
        "${source_struct_string_packet_file}" "${source_struct_string_packet_identity}"
    DEPENDS ${source_struct_string_packet_writer} "${source_struct_string_packet_driver}" "${source_struct_string_packet_shape}"
        "${source_struct_string_packet_fixture}/root.xr"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "Write the complete original Struct/String CLOSED packet and full identity without runtime calls"
    VERBATIM)
add_custom_target(${source_struct_string_packet_outputs}
    DEPENDS "${source_struct_string_packet_file}" "${source_struct_string_packet_identity}")
add_executable(${source_struct_string_packet_runner} "${source_struct_string_packet_driver}" "${source_struct_string_packet_shape}")
target_link_libraries(${source_struct_string_packet_runner} PRIVATE xray_xir_vm)
# PUBLIC provider dependencies may still include Source. This packet-only
# receiver request path does not qualify physical SourceDelete.
add_dependencies(${source_struct_string_packet_runner} ${source_struct_string_packet_outputs})
foreach(source_struct_string_packet_target IN ITEMS ${source_struct_string_packet_writer} ${source_struct_string_packet_runner})
    target_include_directories(${source_struct_string_packet_target} BEFORE PRIVATE
        "${SOURCE_STRUCT_STRING_CURRENT_FORMAL_ROOT}/tests/unit/xir"
        "${SOURCE_STRUCT_STRING_CURRENT_FORMAL_ROOT}/src" "${CMAKE_CURRENT_LIST_DIR}")
    set_target_properties(${source_struct_string_packet_target} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_definitions(${source_struct_string_packet_target} PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(${source_struct_string_packet_target} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${source_struct_string_packet_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME ${source_struct_string_packet_runner} COMMAND ${source_struct_string_packet_runner}
    "${source_struct_string_packet_file}" "${source_struct_string_packet_identity}")
set_tests_properties(${source_struct_string_packet_runner} PROPERTIES
    WORKING_DIRECTORY "${source_struct_string_packet_material}" TIMEOUT 120 PROCESSORS 1 RUN_SERIAL TRUE
    LABELS "unit;xir;program-consumer;struct;string;ownership;packet;source-free;vm;normal-only;current-interface-candidate")
