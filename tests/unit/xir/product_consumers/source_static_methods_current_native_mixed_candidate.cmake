# NOT_REGISTERED / NOT_RUN. Public Git reference 9031dbc is not an adopted producer.
# Checked 28/73 and runtime 23/29/30 require fresh matching product and generated C.
# Include only after Root publishes and binds the same immutable provider/material guard.
# This file supplies no replacement guard; create a fresh dedicated build directory.
set(source_static_native_candidate "${CMAKE_CURRENT_LIST_DIR}")
set(SOURCE_STATIC_CURRENT_FORMAL_ROOT "${CMAKE_SOURCE_DIR}" CACHE PATH
    "Immutable provider owning product libraries, embedded helpers, src and stdlib")
set(source_static_native_fixture "${source_static_native_candidate}/fixtures/static_methods")
set(source_static_native_generated "${CMAKE_BINARY_DIR}/generated/current-static-native-r1")
set(source_static_native_private "${source_static_native_generated}/private-source")
set(source_static_native_material "${source_static_native_generated}/material")
set(source_static_native_writer test_source_static_methods_current_native_writer)
set(source_static_native_runner test_source_static_methods_current_native_mixed)
set(source_static_native_driver "${source_static_native_candidate}/test_source_static_methods_current_native_mixed.c")
set(source_static_native_support "${source_static_native_candidate}/native_mixed_support.h")
if(NOT TARGET xray_xir_source_product OR NOT TARGET xray_xir_vm)
    message(FATAL_ERROR "Candidate requires the provider SourceProduct and public VM targets")
endif()
foreach(source_static_native_helper IN ITEMS xir_source_program_compile_owner.h xir_runtime_allocations.h)
    if(NOT EXISTS "${SOURCE_STATIC_CURRENT_FORMAL_ROOT}/tests/unit/xir/${source_static_native_helper}")
        message(FATAL_ERROR "Provider helper missing: ${source_static_native_helper}")
    endif()
endforeach()
if(NOT EXISTS "${source_static_native_fixture}/root.xr")
    message(FATAL_ERROR "Complete original 220-byte static-method fixture is required")
endif()
add_executable(${source_static_native_writer} "${source_static_native_driver}" "${source_static_native_support}")
target_compile_definitions(${source_static_native_writer} PRIVATE XR_SOURCE_STATIC_NATIVE_PACKET_WRITER=1)
set(source_static_native_c "${source_static_native_material}/actual_payload.c")
set(source_static_native_proof "${source_static_native_material}/actual_proof.bin")
set(source_static_native_identity "${source_static_native_material}/actual_identity.bin")
set(source_static_native_binding "${source_static_native_material}/payload_binding.c")
# The writer exclusively creates these four leaves once. Failure removes only its own leaves.
add_custom_command(
    OUTPUT "${source_static_native_c}" "${source_static_native_proof}"
        "${source_static_native_identity}" "${source_static_native_binding}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${source_static_native_private}" "${source_static_native_material}"
    COMMAND $<TARGET_FILE:${source_static_native_writer}>
        "${source_static_native_private}" "${source_static_native_fixture}"
        "${SOURCE_STATIC_CURRENT_FORMAL_ROOT}/stdlib"
        "${source_static_native_c}" "${source_static_native_proof}"
        "${source_static_native_identity}" "${source_static_native_binding}"
    DEPENDS ${source_static_native_writer} "${source_static_native_driver}"
        "${source_static_native_support}" "${source_static_native_fixture}/root.xr"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "Emit actual C and matching authentic Closed packet/identity on the normal path"
    VERBATIM)
add_executable(${source_static_native_runner} "${source_static_native_driver}" "${source_static_native_support}"
    "${source_static_native_c}" "${source_static_native_binding}")
target_link_libraries(${source_static_native_writer} PRIVATE xray_xir_source_product)
# VM still links native_cache, Source and parser; physical SourceDelete remains OPEN.
target_link_libraries(${source_static_native_runner} PRIVATE xray_xir_vm)
foreach(source_static_native_target IN ITEMS ${source_static_native_writer} ${source_static_native_runner})
    target_include_directories(${source_static_native_target} BEFORE PRIVATE
        "${SOURCE_STATIC_CURRENT_FORMAL_ROOT}/tests/unit/xir"
        "${SOURCE_STATIC_CURRENT_FORMAL_ROOT}/src"
        "${source_static_native_candidate}")
    set_target_properties(${source_static_native_target} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_definitions(${source_static_native_target} PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(${source_static_native_target} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${source_static_native_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
foreach(source_static_native_mode IN ITEMS 1 2 3)
    add_test(NAME ${source_static_native_runner}_mode${source_static_native_mode}
        COMMAND ${source_static_native_runner} "${source_static_native_mode}"
        "${source_static_native_proof}" "${source_static_native_identity}" "${source_static_native_c}")
    set_tests_properties(${source_static_native_runner}_mode${source_static_native_mode} PROPERTIES
        TIMEOUT 120 PROCESSORS 1 WORKING_DIRECTORY "${source_static_native_material}"
        LABELS "unit;xir;program-consumer;class;static-method;ownership;packet;native;mixed;current-interface-candidate;normal-only")
endforeach()
