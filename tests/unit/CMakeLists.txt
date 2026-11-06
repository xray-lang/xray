# Unit tests for xray
# All unit tests are managed here (no glob in root CMakeLists.txt)

# Child and standalone gates share one interpreter discovery in this build.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_subdirectory(framework)
include(${CMAKE_CURRENT_SOURCE_DIR}/core/identity_sha256.cmake)

# Literal semantic vectors are derived independently of compiler libraries.
add_test(NAME test_program_identity_vectors
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/program/derive_identity_vectors.py
        --output ${CMAKE_CURRENT_BINARY_DIR}/program-identity-vectors.json)
add_test(NAME test_source_closure_identity_vectors
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/program/derive_frontend_closure_vector.py
        --output ${CMAKE_CURRENT_BINARY_DIR}/source-closure-identity-vectors.json)
set_tests_properties(test_program_identity_vectors test_source_closure_identity_vectors PROPERTIES
    LABELS "unit;semantic;program-identity;self-test")
add_test(NAME test_semantic_probe_identity_vector
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/plan/derive_semantic_probe_vector.py
        --output ${CMAKE_CURRENT_BINARY_DIR}/semantic-probe-identity-vector.json)
set_tests_properties(test_semantic_probe_identity_vector PROPERTIES
    LABELS "unit;semantic;program-identity;self-test")

# ============================================================================
# Macro for adding independent syntax and shared-kernel tests
# ============================================================================

# Clang's Windows ASan instrumentation expands analyzer/VM recursion far
# beyond the PE default stack.  Keep the sanitizer lane representative by
# reserving the same explicit stack budget for every unit-test executable;
# otherwise unrelated deep-path tests die in __chkstk before ASan can report
# a memory violation.  Directory link options also cover the few standalone
# unit-test targets that do not use add_xray_unit_test().
if(ENABLE_ASAN AND (MSVC OR CMAKE_C_SIMULATE_ID STREQUAL "MSVC"))
    add_link_options("LINKER:/STACK:16777216")
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/base/request_resources.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/base/compiler_arena.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/base/compiler_producer.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/base/compiler_parser.cmake)

include(${CMAKE_CURRENT_SOURCE_DIR}/module/package_compile_resources.cmake)
add_subdirectory(base/hashmap_owner)
if(NOT WIN32)
    add_subdirectory(base/file_read_unix)
    add_subdirectory(cli/publication_posix)
    if(XR_XIR_POSIX_SDK_ARCHITECTURE)
        add_subdirectory(cli/native_posix)
    endif()
endif()
add_subdirectory(xir/compile_owner)
add_subdirectory(xir/type_clone_control)
add_subdirectory(xir/p09_reference)
add_subdirectory(xir/bounded_call)
add_subdirectory(shared/native_admission)
add_subdirectory(shared/bits_wide)
add_subdirectory(shared/core_intrinsic_owner)
add_subdirectory(cli/native_ready)

add_subdirectory(module/declaration_owner)
add_subdirectory(module/stdlib_source_owner)
if(WIN32)
    add_subdirectory(os/thread_join_owner)
    add_subdirectory(module/module_owner)
    add_subdirectory(module/project_owner)
    add_executable(test_cli_source_owner cli/source_owner/test_cli_source_owner.c)
    target_link_libraries(test_cli_source_owner PRIVATE xray_source_compile bcrypt)
    target_compile_options(test_cli_source_owner PRIVATE /W4 /WX /utf-8)
    add_test(NAME test_cli_source_owner COMMAND ${XRAY_PYTHON} -X utf8
        ${CMAKE_CURRENT_SOURCE_DIR}/cli/source_owner/test_cli_source_owner.py
        $<TARGET_FILE:test_cli_source_owner> ${PROJECT_SOURCE_DIR}/stdlib)
    set_tests_properties(test_cli_source_owner PROPERTIES TIMEOUT 300 COST 40
        RUN_SERIAL TRUE LABELS "unit;cli;xir;ownership;budget")
endif()
include(${CMAKE_CURRENT_SOURCE_DIR}/cli/xir_target_authority.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cli/native_projection_owner.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cli/native_invocation_owner.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cli/native_operation_owner.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cli/local_toolchain_owner.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cli/toolchain_config.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/sdk_installed_native_owner.cmake)
add_subdirectory(cli/pe_version_owner)
if(WIN32)
    set(ADMISSION_SDK "${CMAKE_BINARY_DIR}/xir-runtime-sdk")
    set(ADMISSION_MSVC "${XIR_INVOCATION_MSVC}")
    set(ADMISSION_LINKER "${XIR_INVOCATION_LINKER}")
    add_subdirectory(cli/native_admission_owner)
endif()
include(${CMAKE_CURRENT_SOURCE_DIR}/cli/workspace_owner.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cli/source_paths_owner.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cli/run_owner.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cli/check_owner.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cli/build_source_owner.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cli/public_source_program.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/api/source_product_text.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/api/source_compiler.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/api/source_generated_c.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/api/source_public.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cli/entry_oracles.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/api/source_submission.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/base/windows_path_owner.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/cgen_bytes_owner.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/test_declarations_owner/gates.cmake)

macro(add_xray_unit_test name source)
    add_executable(${name} ${source} ${ARGN})
    target_include_directories(${name} PRIVATE
        ${XRAY_COMMON_INCLUDES}
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(${name} PRIVATE xray_compiler_parser)
    add_test(NAME ${name} COMMAND $<TARGET_FILE:${name}>)
endmacro()

if(WIN32)
    add_executable(test_windows_utf8 base/test_windows_utf8.c)
    target_link_libraries(test_windows_utf8 PRIVATE xray_compile_resources)
    target_include_directories(test_windows_utf8 PRIVATE ${CMAKE_SOURCE_DIR}/src)
    if(MSVC)
        target_compile_options(test_windows_utf8 PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(test_windows_utf8 PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME test_windows_utf8 COMMAND test_windows_utf8)
    set_tests_properties(test_windows_utf8 PROPERTIES LABELS "unit;ownership;windows")
    add_executable(test_windows_paths base/test_windows_paths.c
        ${CMAKE_SOURCE_DIR}/src/base/xfileio.c
        ${CMAKE_SOURCE_DIR}/src/os/win/fs_win.c
        ${CMAKE_SOURCE_DIR}/src/os/win/fs_mutation_win.c
        ${CMAKE_SOURCE_DIR}/src/os/win/file_read_win.c)
    target_link_libraries(test_windows_paths PRIVATE xray_compile_resources)
    target_sources(test_windows_paths PRIVATE ${CMAKE_SOURCE_DIR}/src/base/xio_policy.c)
    target_include_directories(test_windows_paths PRIVATE ${CMAKE_SOURCE_DIR}/src)
    if(MSVC)
        target_compile_options(test_windows_paths PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(test_windows_paths PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME test_windows_paths
        COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/base/test_windows_paths.py
            $<TARGET_FILE:test_windows_paths>)
    set_tests_properties(test_windows_paths PROPERTIES LABELS "unit;ownership;windows;filesystem")
    add_test(NAME test_windows_unicode_paths
        COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/cli/test_windows_unicode_paths.py
            --xray $<TARGET_FILE:xray>)
    set_tests_properties(test_windows_unicode_paths PROPERTIES
        LABELS "integration;cli;windows;vm;aot" TIMEOUT 600 COST 180
        ENVIRONMENT "XRAY_STDLIB_PATH=${CMAKE_SOURCE_DIR}/stdlib")
    add_executable(test_file_read_windows base/test_file_read_windows.c
        ${CMAKE_SOURCE_DIR}/src/base/xio_policy.c
        ${CMAKE_SOURCE_DIR}/src/os/win/file_read_win.c)
    if(MSVC AND CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_file_read_windows PRIVATE /experimental:c11atomics)
    endif()
    target_include_directories(test_file_read_windows PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_executable(test_declaration_file_windows base/test_declaration_file_windows.c
        ${CMAKE_SOURCE_DIR}/src/base/xio_policy.c
        ${CMAKE_SOURCE_DIR}/src/base/xtoml.c
        ${CMAKE_SOURCE_DIR}/src/os/win/file_read_win.c
        ${CMAKE_SOURCE_DIR}/src/module/xdeclaration_manifest.c
        ${CMAKE_SOURCE_DIR}/src/module/xdeclaration_load.c)
    if(MSVC AND CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_declaration_file_windows PRIVATE /experimental:c11atomics)
    endif()
    target_include_directories(test_declaration_file_windows PRIVATE ${CMAKE_SOURCE_DIR}/src)
    foreach(file_test test_file_read_windows test_declaration_file_windows)
        if(MSVC)
            target_compile_options(${file_test} PRIVATE /W4 /WX)
        else()
            target_compile_options(${file_test} PRIVATE -Wall -Wextra -Werror)
        endif()
    endforeach()
    add_test(NAME test_file_read_windows
        COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/base/test_file_read_windows.py
            $<TARGET_FILE:test_file_read_windows>)
    set_tests_properties(test_file_read_windows PROPERTIES LABELS "unit;metadata;ownership;windows")
    add_test(NAME test_declaration_file_windows
        COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/base/test_declaration_file_windows.py
            $<TARGET_FILE:test_declaration_file_windows>)
    set_tests_properties(test_declaration_file_windows PROPERTIES LABELS "unit;metadata;ownership;windows")

    add_executable(test_random_windows stdlib/test_random_windows.c
        ${CMAKE_SOURCE_DIR}/src/os/win/random_win.c)
    target_include_directories(test_random_windows PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_random_windows PRIVATE bcrypt)
    add_test(NAME test_random_windows COMMAND test_random_windows)
    set_tests_properties(test_random_windows PROPERTIES LABELS "unit;crypto;runtime;windows")
endif()

# Legacy compiler-owner fixtures retain their shared object aggregate.
# Its native-entry registry is generated alongside immutable source metadata.

function(add_xray_python_unit_test name file)
    add_test(NAME ${name}
        COMMAND ${XRAY_PYTHON} -m unittest discover
            -s ${CMAKE_SOURCE_DIR}/tests/lib/tests -p ${file})
    set_tests_properties(${name} PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        LABELS "unit;test-infrastructure;self-test")
endfunction()

add_xray_python_unit_test(test_build_tree_lock test_buildlock.py)
add_xray_python_unit_test(test_tiered_test_runner test_t_runner.py)
add_xray_python_unit_test(test_regression_report test_regression_report.py)
add_xray_python_unit_test(test_tsan_focused_runner test_tsan_focused.py)
add_xray_python_unit_test(test_canonical_program_test_profile
    test_canonical_program_test_profile.py)
add_xray_python_unit_test(xr_program_h2_backend_differential_self_test
    test_h2_backend_differential.py)

add_executable(test_xtoml base/test_xtoml.c ${CMAKE_SOURCE_DIR}/src/base/xtoml.c
    ${CMAKE_SOURCE_DIR}/src/base/xio_policy.c ${CMAKE_SOURCE_DIR}/src/base/xcompile_resources.c)
target_include_directories(test_xtoml PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(MSVC)
    target_compile_options(test_xtoml PRIVATE /W4 /WX)
else()
    target_compile_options(test_xtoml PRIVATE -Wall -Wextra -Werror)
endif()
add_executable(test_xtoml_allocations base/test_xtoml_allocations.c ${CMAKE_SOURCE_DIR}/src/base/xio_policy.c)
if(MSVC AND CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(test_xtoml PRIVATE /experimental:c11atomics)
    target_compile_options(test_xtoml_allocations PRIVATE /experimental:c11atomics)
endif()
target_include_directories(test_xtoml_allocations PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(MSVC)
    target_compile_options(test_xtoml_allocations PRIVATE /W4 /WX)
else()
    target_compile_options(test_xtoml_allocations PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xtoml_allocations COMMAND test_xtoml_allocations)
set_tests_properties(test_xtoml_allocations PROPERTIES LABELS "unit;metadata;ownership")
add_test(NAME test_xtoml COMMAND test_xtoml)
set_tests_properties(test_xtoml PROPERTIES LABELS "unit;metadata;ownership")

add_executable(test_declaration_manifest module/test_declaration_manifest.c
    ${CMAKE_SOURCE_DIR}/src/base/xio_policy.c
    ${CMAKE_SOURCE_DIR}/src/base/xtoml.c
    ${CMAKE_SOURCE_DIR}/src/module/xdeclaration_manifest.c)
if(MSVC AND CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(test_declaration_manifest PRIVATE /experimental:c11atomics)
endif()
target_include_directories(test_declaration_manifest PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(MSVC)
    target_compile_options(test_declaration_manifest PRIVATE /W4 /WX)
else()
    target_compile_options(test_declaration_manifest PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_declaration_manifest COMMAND test_declaration_manifest)
set_tests_properties(test_declaration_manifest PROPERTIES LABELS "unit;metadata;ownership")

# ============================================================================
# value/ — Value system
# ============================================================================

add_executable(test_xir_effects_source xir/test_xir_effects_source.c)
target_link_libraries(test_xir_effects_source PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_xir_effects_source PRIVATE XR_EFFECT_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_effect_source")
if(MSVC)
    target_compile_options(test_xir_effects_source PRIVATE /W4 /WX)
else()
    target_compile_options(test_xir_effects_source PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_effects_source COMMAND test_xir_effects_source)
set_tests_properties(test_xir_effects_source PROPERTIES LABELS "unit;xir;metadata;ownership")
add_test(NAME test_xir_source_fixed_callable_boundary COMMAND test_xir_effects_source --qualified-boundary)
set_tests_properties(test_xir_source_fixed_callable_boundary PROPERTIES LABELS "unit;xir;metadata;ownership" TIMEOUT 60)
add_test(NAME test_xir_source_qualified_forwarding COMMAND test_xir_effects_source --qualified-forwarding)
set_tests_properties(test_xir_source_qualified_forwarding PROPERTIES LABELS "unit;xir;metadata;ownership" TIMEOUT 60)
set(XIR_EFFECT_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_effect_fixture.c)
add_custom_command(OUTPUT ${XIR_EFFECT_GENERATED}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND $<TARGET_FILE:test_xir_effects_source> ${XIR_EFFECT_GENERATED}
    DEPENDS test_xir_effects_source ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_effect_source/root.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_effect_source/methods.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_effect_source/default_selector.xr
    VERBATIM)
add_executable(test_xir_effects_native xir/test_xir_effects_native.c ${XIR_EFFECT_GENERATED})
target_link_libraries(test_xir_effects_native PRIVATE xray_xir_scalar)
add_executable(test_xir_effects_mixed xir/test_xir_effects_mixed.c ${XIR_EFFECT_GENERATED})
target_link_libraries(test_xir_effects_mixed PRIVATE xray_xir_vm)
add_executable(test_xir_effects xir/test_xir_effects.c)
target_link_libraries(test_xir_effects PRIVATE xray_xir)
if(MSVC)
    target_compile_options(test_xir_effects PRIVATE /W4 /WX)
else()
    target_compile_options(test_xir_effects PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_effects COMMAND test_xir_effects)
set_tests_properties(test_xir_effects PROPERTIES LABELS "unit;xir;metadata;ownership")
add_test(NAME test_xir_effect_error_query_work COMMAND test_xir_effects --error-query-work)
set_tests_properties(test_xir_effect_error_query_work PROPERTIES
    LABELS "unit;xir;metadata;ownership;bounded-query" TIMEOUT 60)
add_executable(test_xir_stages xir/test_xir_stages.c)
add_executable(test_xir_interfaces xir/test_xir_interfaces.c)
target_link_libraries(test_xir_interfaces PRIVATE xray_xir)
if(MSVC)
    target_compile_options(test_xir_interfaces PRIVATE /W4 /WX)
else()
    target_compile_options(test_xir_interfaces PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_interfaces COMMAND test_xir_interfaces)
set_tests_properties(test_xir_interfaces PROPERTIES LABELS "unit;xir;metadata;ownership")
target_link_libraries(test_xir_stages PRIVATE xray_xir)
if(MSVC)
    target_compile_options(test_xir_stages PRIVATE /W4 /WX)
else()
    target_compile_options(test_xir_stages PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_stages COMMAND test_xir_stages)
set_tests_properties(test_xir_stages PROPERTIES LABELS "unit;xir;metadata;ownership")
add_test(NAME l3_xir_array_frame_limits COMMAND test_xir_stages --legacy-array-frame-limits)
set_tests_properties(l3_xir_array_frame_limits PROPERTIES LABELS "l3;xir;legacy-resource;redlist" TIMEOUT 60)
add_executable(test_xir_allocations xir/test_xir_allocations.c
    ${CMAKE_SOURCE_DIR}/src/lir/xi_cgen_verify_output.c
    ${CMAKE_SOURCE_DIR}/src/shared/xnative_declaration.c)
target_link_libraries(test_xir_allocations PRIVATE xray_xir_scalar)
target_include_directories(test_xir_allocations PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(MSVC)
    target_compile_options(test_xir_allocations PRIVATE /W4 /WX)
else()
    target_compile_options(test_xir_allocations PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_allocations
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/xir/run_allocation_case_faults.py
        --executable $<TARGET_FILE:test_xir_allocations>
        --source-root ${CMAKE_SOURCE_DIR} --build-root ${CMAKE_BINARY_DIR} --mode aggregate)
set_tests_properties(test_xir_allocations PROPERTIES LABELS "unit;xir;memory;ownership")
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/allocation_feedback.cmake)

add_executable(test_xir_execution xir/test_xir_execution.c)
add_executable(test_xir_locals xir/test_xir_locals.c)
target_link_libraries(test_xir_locals PRIVATE xray_xir_vm xray_xir_cgen)
set(XIR_LOCAL_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_local_fixture.c)
add_custom_command(OUTPUT ${XIR_LOCAL_GENERATED}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND $<TARGET_FILE:test_xir_locals> ${XIR_LOCAL_GENERATED}
    DEPENDS test_xir_locals VERBATIM)
add_executable(test_xir_local_native xir/test_xir_local_native.c ${XIR_LOCAL_GENERATED})
target_link_libraries(test_xir_local_native PRIVATE xray_xir_scalar)
add_executable(test_xir_generics xir/test_xir_generics.c)
target_link_libraries(test_xir_generics PRIVATE xray_xir_vm)
add_executable(test_xir_checked xir/test_xir_checked.c)
target_link_libraries(test_xir_checked PRIVATE xray_xir)
add_executable(test_xir_checked_allocations xir/test_xir_checked_allocations.c
    ${CMAKE_SOURCE_DIR}/src/base/xsha256.c
    ${CMAKE_SOURCE_DIR}/src/shared/xnative_declaration.c)
target_include_directories(test_xir_checked_allocations PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_xir_execution PRIVATE xray_xir_vm)
add_executable(test_xir_cleanup_driver xir/test_xir_cleanup_driver.c)
target_link_libraries(test_xir_cleanup_driver PRIVATE xray_xir_scalar)
add_test(NAME test_xir_cleanup_fatal COMMAND ${XRAY_PYTHON}
    ${CMAKE_CURRENT_SOURCE_DIR}/xir/check_xir_cleanup_fatal.py $<TARGET_FILE:test_xir_cleanup_driver>)
set_tests_properties(test_xir_cleanup_fatal PROPERTIES LABELS "unit;xir;ownership")
add_test(NAME test_xir_source_cleanup_fatal COMMAND ${XRAY_PYTHON}
    ${CMAKE_CURRENT_SOURCE_DIR}/xir/check_xir_source_cleanup_fatal.py
    $<TARGET_FILE:test_xir_cleanup_source>)
set_tests_properties(test_xir_source_cleanup_fatal PROPERTIES LABELS "unit;xir;ownership;abi;execution")
add_executable(test_xir_calls xir/test_xir_calls.c)
target_link_libraries(test_xir_calls PRIVATE xray_xir_vm)
add_executable(test_xir_emit xir/test_xir_emit.c)
target_link_libraries(test_xir_emit PRIVATE xray_xir_cgen)
add_executable(test_xir_emit_calls xir/test_xir_emit_calls.c)
target_link_libraries(test_xir_emit_calls PRIVATE xray_xir_cgen)
set(XIR_CALL_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_call_fixture.c)
add_custom_command(OUTPUT ${XIR_CALL_GENERATED}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND $<TARGET_FILE:test_xir_emit_calls> ${XIR_CALL_GENERATED}
    DEPENDS test_xir_emit_calls
    VERBATIM)
add_executable(test_xir_call_native xir/test_xir_call_native.c ${XIR_CALL_GENERATED})
target_link_libraries(test_xir_call_native PRIVATE xray_xir_scalar)
target_sources(test_xir_calls PRIVATE ${XIR_CALL_GENERATED})
include("${CMAKE_CURRENT_SOURCE_DIR}/xir/calls_owned_receive.cmake")
set(XIR_SCALAR_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_scalar_fixture.c)
add_custom_command(OUTPUT ${XIR_SCALAR_GENERATED}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND $<TARGET_FILE:test_xir_emit> ${XIR_SCALAR_GENERATED}
    DEPENDS test_xir_emit
    VERBATIM)
add_executable(test_xir_native xir/test_xir_native.c ${XIR_SCALAR_GENERATED})
target_link_libraries(test_xir_native PRIVATE xray_xir_scalar)
# Complete panic outcomes keep message ownership independent of result values.
add_executable(test_xir_panic_carrier xir/test_xir_panic_carrier.c xir/xir_legacy_call_provider.c)
target_link_libraries(test_xir_panic_carrier PRIVATE xray_xir xray_xir_scalar)
# These implementations are included under the runtime allocation macros,
# never compiled as additional uncounted translation units.
set_property(SOURCE xir/test_xir_panic_carrier.c APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/xir/xxir_task_budget.c"
    "${CMAKE_SOURCE_DIR}/src/xir/xxir_task.c"
    "${CMAKE_SOURCE_DIR}/src/xir/xxir_task_outcome.inc.c")
if(MSVC)
    target_compile_options(test_xir_panic_carrier PRIVATE /W4 /WX)
else()
    target_compile_options(test_xir_panic_carrier PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_panic_carrier COMMAND $<TARGET_FILE:test_xir_panic_carrier>)
add_test(NAME test_xir_panic_carrier_fatal COMMAND ${XRAY_PYTHON}
    ${CMAKE_CURRENT_SOURCE_DIR}/xir/check_xir_panic_carrier_fatal.py $<TARGET_FILE:test_xir_panic_carrier>)
add_test(NAME test_xir_panic_carrier_wire_vector COMMAND ${XRAY_PYTHON}
    ${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_panic_carrier_vector.py
    --output ${CMAKE_CURRENT_BINARY_DIR}/generated/panic-carrier-wire-vector.json)
set_tests_properties(test_xir_panic_carrier test_xir_panic_carrier_fatal test_xir_panic_carrier_wire_vector
    PROPERTIES LABELS "unit;xir;ownership;abi")

add_executable(test_xir_cell_owner_values xir/test_xir_cell_owner_values.c)
target_link_libraries(test_xir_cell_owner_values PRIVATE xray_xir_scalar)
add_test(NAME test_xir_cell_owner_values COMMAND test_xir_cell_owner_values)
set_tests_properties(test_xir_cell_owner_values PROPERTIES LABELS "unit;xir;ownership")
include("${CMAKE_CURRENT_LIST_DIR}/xir/object_storage_prefix.cmake")
add_executable(test_xir_values xir/test_xir_values.c)
target_link_libraries(test_xir_values PRIVATE xray_xir_scalar)
add_executable(test_xir_instances xir/test_xir_instances.c xir/xir_instance_retirement_probe.c)
target_link_libraries(test_xir_instances PRIVATE xray_xir_scalar xray_xir)
add_test(NAME test_xir_instance_invalid_slot_views COMMAND test_xir_instances --invalid-slot-views)
set_tests_properties(test_xir_instance_invalid_slot_views PROPERTIES LABELS "unit;xir;ownership" TIMEOUT 30)
add_executable(test_xir_emit_program xir/test_xir_emit_program.c)
target_link_libraries(test_xir_emit_program PRIVATE xray_xir_cgen)
set(XIR_PROGRAM_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_program_fixture.c)
add_custom_command(OUTPUT ${XIR_PROGRAM_GENERATED}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND $<TARGET_FILE:test_xir_emit_program> ${XIR_PROGRAM_GENERATED}
    DEPENDS test_xir_emit_program
    VERBATIM)
add_executable(test_xir_program_native xir/test_xir_program_native.c ${XIR_PROGRAM_GENERATED})
target_link_libraries(test_xir_program_native PRIVATE xray_xir_scalar)
add_executable(test_xir_program_vm xir/test_xir_program_vm.c ${XIR_PROGRAM_GENERATED})
target_link_libraries(test_xir_program_vm PRIVATE xray_xir_vm)
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(test_xir_values PRIVATE Threads::Threads)
endif()
add_executable(test_xir_array_grow_budget xir/test_xir_array_grow_budget.c)
target_include_directories(test_xir_array_grow_budget PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(test_xir_array_grow_budget PRIVATE xray_xir_scalar)
if(MSVC)
    target_compile_options(test_xir_array_grow_budget PRIVATE /W4 /WX)
else()
    target_compile_options(test_xir_array_grow_budget PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_array_grow_budget COMMAND test_xir_array_grow_budget)
set_tests_properties(test_xir_array_grow_budget PROPERTIES TIMEOUT 120 LABELS "unit;xir;array;ownership;budget")
add_executable(test_xir_value_allocations xir/test_xir_value_allocations.c)
target_include_directories(test_xir_value_allocations PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_xir_value_allocations PRIVATE xray_xir_scalar)
add_test(NAME test_xir_value_path_replacement COMMAND test_xir_value_allocations --path-replacement-normal)
add_test(NAME test_xir_value_path_replacement_resources COMMAND test_xir_value_allocations --path-replacement)
set_tests_properties(test_xir_value_path_replacement test_xir_value_path_replacement_resources PROPERTIES TIMEOUT 120 LABELS "unit;xir;ownership;resources;fault-injection")
# Owned close/copy tests use the production thread wrapper and join every handle.
target_include_directories(test_xir_value_allocations PRIVATE ${CMAKE_SOURCE_DIR}/src/base)
target_include_directories(test_xir_array_grow_budget PRIVATE ${CMAKE_SOURCE_DIR}/src/base)
if(WIN32)
    target_sources(test_xir_value_allocations PRIVATE ${CMAKE_SOURCE_DIR}/src/os/win/thread_win.c)
    target_sources(test_xir_array_grow_budget PRIVATE ${CMAKE_SOURCE_DIR}/src/os/win/thread_win.c)
else()
    target_sources(test_xir_value_allocations PRIVATE ${CMAKE_SOURCE_DIR}/src/os/unix/thread_unix.c)
    target_sources(test_xir_array_grow_budget PRIVATE ${CMAKE_SOURCE_DIR}/src/os/unix/thread_unix.c)
    find_package(Threads REQUIRED)
    target_link_libraries(test_xir_value_allocations PRIVATE Threads::Threads)
    target_link_libraries(test_xir_array_grow_budget PRIVATE Threads::Threads)
endif()
add_executable(test_xir_emit_strings xir/test_xir_emit_strings.c)
target_link_libraries(test_xir_emit_strings PRIVATE xray_xir_cgen)
set(XIR_STRING_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_string_fixture.c)
add_custom_command(OUTPUT ${XIR_STRING_GENERATED}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND $<TARGET_FILE:test_xir_emit_strings> ${XIR_STRING_GENERATED}
    DEPENDS test_xir_emit_strings
    VERBATIM)
add_executable(test_xir_string_native xir/test_xir_string_native.c ${XIR_STRING_GENERATED})
target_sources(test_xir_string_native PRIVATE xir/xir_string_instance_probe.c)
target_link_libraries(test_xir_string_native PRIVATE xray_xir_scalar)
add_executable(test_xir_string_vm xir/test_xir_string_vm.c ${XIR_STRING_GENERATED})
target_sources(test_xir_string_vm PRIVATE xir/xir_string_instance_probe.c)
target_link_libraries(test_xir_string_vm PRIVATE xray_xir_vm)
include("${CMAKE_CURRENT_SOURCE_DIR}/xir/string_allocation_owner.cmake")
add_executable(test_xir_emit_output xir/test_xir_emit_output.c)
target_link_libraries(test_xir_emit_output PRIVATE xray_xir_cgen)
set(XIR_OUTPUT_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_output_fixture.c)
add_custom_command(OUTPUT ${XIR_OUTPUT_GENERATED}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND $<TARGET_FILE:test_xir_emit_output> ${XIR_OUTPUT_GENERATED}
    DEPENDS test_xir_emit_output
    VERBATIM)
add_executable(test_xir_output_native xir/test_xir_output_native.c ${XIR_OUTPUT_GENERATED})
target_link_libraries(test_xir_output_native PRIVATE xray_xir_scalar)
add_executable(test_xir_output_vm xir/test_xir_output_vm.c)
target_link_libraries(test_xir_output_vm PRIVATE xray_xir_vm)
add_executable(test_xir_output_status xir/test_xir_output_status.c)
target_link_libraries(test_xir_output_status PRIVATE xray_xir_scalar)
if(MSVC)
    target_compile_options(test_xir_output_status PRIVATE /W4 /WX)
else()
    target_compile_options(test_xir_output_status PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_output_status COMMAND test_xir_output_status)
set_tests_properties(test_xir_output_status PROPERTIES LABELS "unit;xir;ownership;abi")

add_executable(test_xir_source xir/test_xir_source.c)
target_link_libraries(test_xir_source PRIVATE xray_xir_source xray_xir_vm)
add_executable(test_xir_source_product xir/test_xir_source_product.c)
add_test(NAME test_xir_native_projection_policy COMMAND ${XRAY_PYTHON}
    ${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_native_projection_vectors.py)
set_tests_properties(test_xir_native_projection_policy PROPERTIES LABELS "unit;xir;abi;generated-c" TIMEOUT 30)
target_link_libraries(test_xir_source_product PRIVATE xray_xir_native_projection)
if(MSVC)
    target_compile_options(test_xir_source_product PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_source_product PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_source_product COMMAND ${XRAY_PYTHON}
    ${CMAKE_CURRENT_SOURCE_DIR}/xir/test_xir_source_product.py
    $<TARGET_FILE:test_xir_source_product> ${CMAKE_SOURCE_DIR}/stdlib)
set_tests_properties(test_xir_source_product PROPERTIES
    LABELS "unit;xir;ownership;abi;execution")
foreach(source_case retired array)
    add_test(NAME test_xir_source_product_original_${source_case} COMMAND ${XRAY_PYTHON}
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/test_xir_source_product.py
        $<TARGET_FILE:test_xir_source_product> ${CMAKE_SOURCE_DIR}/stdlib --case ${source_case})
    set_tests_properties(test_xir_source_product_original_${source_case} PROPERTIES
        LABELS "unit;xir;ownership;abi;execution")
endforeach()
add_test(NAME test_xir_source_product_allocations COMMAND ${XRAY_PYTHON}
    ${CMAKE_CURRENT_SOURCE_DIR}/xir/test_xir_source_product.py
    $<TARGET_FILE:test_xir_source_product> ${CMAKE_SOURCE_DIR}/stdlib --scan)
set_tests_properties(test_xir_source_product_allocations PROPERTIES
    LABELS "unit;xir;ownership;abi;execution" TIMEOUT 600 COST 80)
add_executable(test_xir_source_product_linked xir/test_xir_source_product_linked.c)
target_link_libraries(test_xir_source_product_linked PRIVATE xray_xir_native_projection)
if(MSVC)
    target_compile_options(test_xir_source_product_linked PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_source_product_linked PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_source_product_linked COMMAND ${XRAY_PYTHON}
    ${CMAKE_CURRENT_SOURCE_DIR}/xir/test_xir_source_product.py
    $<TARGET_FILE:test_xir_source_product_linked> ${CMAKE_SOURCE_DIR}/stdlib)
set_tests_properties(test_xir_source_product_linked PROPERTIES LABELS "unit;xir;ownership;abi;execution" TIMEOUT 60)
add_test(NAME test_xir_source_product_identity_oracle COMMAND ${XRAY_PYTHON}
    ${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_source_product_identity.py)
set_tests_properties(test_xir_source_product_identity_oracle PROPERTIES LABELS "unit;xir;abi;ownership" TIMEOUT 30)
set(XIR_SOURCE_OWNER_FIXTURE "${CMAKE_SOURCE_DIR}/tests/fixtures/xir_compile_owner")
set(XIR_SOURCE_OWNER_NATIVE "${CMAKE_CURRENT_BINARY_DIR}/xir-source-owner-native.c")
add_custom_command(OUTPUT "${XIR_SOURCE_OWNER_NATIVE}"
    COMMAND $<TARGET_FILE:test_xir_source_product_linked> "${XIR_SOURCE_OWNER_FIXTURE}"
        "${XIR_SOURCE_OWNER_FIXTURE}/root.xr" "${CMAKE_SOURCE_DIR}/stdlib"
        accept "${XIR_SOURCE_OWNER_NATIVE}" multi normal
    DEPENDS test_xir_source_product_linked "${XIR_SOURCE_OWNER_FIXTURE}/root.xr"
        "${XIR_SOURCE_OWNER_FIXTURE}/counter.xr" "${XIR_SOURCE_OWNER_FIXTURE}/label.xr"
    VERBATIM)
add_executable(test_xir_source_product_native xir/test_xir_source_product_native.c "${XIR_SOURCE_OWNER_NATIVE}")
target_link_libraries(test_xir_source_product_native PRIVATE xray_xir_scalar xray_compile_resources)
if(MSVC)
    target_compile_options(test_xir_source_product_native PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_source_product_native PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME test_xir_source_product_vm_modules
    COMMAND test_xir_source_product_linked "${XIR_SOURCE_OWNER_FIXTURE}"
        "${XIR_SOURCE_OWNER_FIXTURE}/root.xr" "${CMAKE_SOURCE_DIR}/stdlib"
        accept "${CMAKE_CURRENT_BINARY_DIR}/xir-source-owner-vm.c" multi normal)
add_test(NAME test_xir_source_product_native COMMAND test_xir_source_product_native)
set_tests_properties(test_xir_source_product_vm_modules test_xir_source_product_native
    PROPERTIES LABELS "unit;xir;ownership;abi;execution" TIMEOUT 60)
target_compile_definitions(test_xir_source PRIVATE XR_SOURCE_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_source")
add_executable(test_xir_source_admission xir/test_xir_source_admission.c)
add_executable(test_xir_source_generics xir/test_xir_source_generics.c)
add_executable(test_xir_source_query xir/test_xir_source_query.c)
add_executable(test_xir_source_packages xir/test_xir_source_packages.c)
target_link_libraries(test_xir_source_packages PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
if(MSVC)
    target_compile_options(test_xir_source_packages PRIVATE /W4 /WX)
else()
    target_compile_options(test_xir_source_packages PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_source_packages COMMAND ${XRAY_PYTHON}
    ${CMAKE_CURRENT_SOURCE_DIR}/xir/run_source_packages.py $<TARGET_FILE:test_xir_source_packages>)
set_tests_properties(test_xir_source_packages PROPERTIES LABELS "unit;xir;metadata;ownership")
set(XIR_PACKAGE_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_package_fixture.c)
add_custom_command(OUTPUT ${XIR_PACKAGE_GENERATED}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/xir/run_source_packages.py
        $<TARGET_FILE:test_xir_source_packages> ${XIR_PACKAGE_GENERATED}
    DEPENDS test_xir_source_packages ${CMAKE_CURRENT_SOURCE_DIR}/xir/run_source_packages.py
    VERBATIM)
add_executable(test_xir_package_native xir/test_xir_package_native.c ${XIR_PACKAGE_GENERATED})
target_link_libraries(test_xir_package_native PRIVATE xray_xir_scalar)
if(MSVC)
    target_compile_options(test_xir_package_native PRIVATE /W4 /WX)
else()
    target_compile_options(test_xir_package_native PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_package_native COMMAND test_xir_package_native)
set_tests_properties(test_xir_package_native PROPERTIES LABELS "unit;xir;execution;ownership;abi")

target_link_libraries(test_xir_source_query PRIVATE xray_xir_source xray_xir_vm)
add_test(NAME test_xir_source_class_definitions COMMAND test_xir_source_query --class-definitions)
set_tests_properties(test_xir_source_class_definitions PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE
    LABELS "unit;xir;ownership;metadata;class")
add_test(NAME test_xir_source_core_syntax COMMAND test_xir_source_query --core-syntax)
set_tests_properties(test_xir_source_core_syntax PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE
    LABELS "unit;xir;ownership;metadata")
add_test(NAME test_xir_source_interface_name_spans COMMAND test_xir_source_query --interface-name-spans)
set_tests_properties(test_xir_source_interface_name_spans PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE
    LABELS "unit;xir;ownership;metadata")

add_executable(test_lsp_source_query xir/test_lsp_source_query.c)
target_link_libraries(test_lsp_source_query PRIVATE xray_lsp_source_query xray_lsp_source_syntax xray_lsp_document_open)
if(MSVC)
    target_compile_options(test_lsp_source_query PRIVATE /W4 /WX)
else()
    target_compile_options(test_lsp_source_query PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_lsp_source_query COMMAND test_lsp_source_query)
set_tests_properties(test_lsp_source_query PROPERTIES LABELS "unit;xir;lsp;ownership;metadata" TIMEOUT 60)

add_executable(test_xir_source_overlay xir/test_xir_source_overlay.c)
target_link_libraries(test_xir_source_overlay PRIVATE xray_xir_source)
if(MSVC)
    target_compile_options(test_xir_source_overlay PRIVATE /W4 /WX)
else()
    target_compile_options(test_xir_source_overlay PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_source_overlay COMMAND test_xir_source_overlay)
set_tests_properties(test_xir_source_overlay PROPERTIES LABELS "unit;xir;ownership;metadata" TIMEOUT 60)

add_executable(test_xir_module_overlay xir/test_xir_module_overlay.c)
target_link_libraries(test_xir_module_overlay PRIVATE xray_xir_source)
if(MSVC)
    target_compile_options(test_xir_module_overlay PRIVATE /W4 /WX)
else()
    target_compile_options(test_xir_module_overlay PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_module_overlay COMMAND test_xir_module_overlay)
set_tests_properties(test_xir_module_overlay PROPERTIES LABELS "unit;xir;ownership;metadata" TIMEOUT 60)

# Count the actual compiler-resource allocator used by the linked Source owner.
add_executable(test_xir_source_text xir/test_xir_source_text.c)
target_link_libraries(test_xir_source_text PRIVATE xray_xir_source)
if(MSVC)
    target_compile_options(test_xir_source_text PRIVATE /W4 /WX)
else()
    target_compile_options(test_xir_source_text PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_source_text COMMAND test_xir_source_text)
set_tests_properties(test_xir_source_text PROPERTIES LABELS "unit;xir;ownership;metadata" TIMEOUT 60)
include("${CMAKE_CURRENT_SOURCE_DIR}/xir/source_ast.cmake")

target_link_libraries(test_xir_source_generics PRIVATE xray_xir_source xray_frontend_format xray_xir_vm)
target_link_libraries(test_xir_source_admission PRIVATE xray_xir_source)
add_executable(test_xir_source_allocations xir/test_xir_source_allocations.c)
target_link_libraries(test_xir_source_allocations PRIVATE xray_xir_source xray_xir xray_xir_scalar)
target_compile_definitions(test_xir_source_allocations PRIVATE XR_SOURCE_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_source")
target_compile_definitions(test_xir_source_allocations PRIVATE XR_CLASS_ARRAY_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_class_array")
set(XIR_SOURCE_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_source_fixture.c)
set(XIR_CHECKED_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_source_fixture.xrc)
add_custom_command(OUTPUT ${XIR_CHECKED_GENERATED}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND $<TARGET_FILE:test_xir_source> --write-checked ${XIR_CHECKED_GENERATED}
    DEPENDS test_xir_source
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_source/root.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_source/alpha.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_source/beta.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_source/measure_contract.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_source/measure_reader.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_source/measure_value.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_source/method_contract.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_source/method_reader.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_source/method_value.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_source/inference_contract.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_source/inference_reader.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_source/inference_value.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_source/method_value_contract.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_source/method_value_reader.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_source/method_value_value.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_source/late_result_contract.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_source/late_result_value.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_source/late_result_reader.xr
        ${CMAKE_SOURCE_DIR}/stdlib/io/output.xr
    VERBATIM)
add_executable(test_xir_packet_vm xir/test_xir_packet_vm.c ${XIR_CHECKED_GENERATED})
target_link_libraries(test_xir_packet_vm PRIVATE xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_xir_packet_vm PRIVATE XR_CHECKED_FIXTURE="${XIR_CHECKED_GENERATED}")
add_custom_command(OUTPUT ${XIR_SOURCE_GENERATED}
    COMMAND $<TARGET_FILE:test_xir_packet_vm> --write-c ${XIR_SOURCE_GENERATED}
    DEPENDS test_xir_packet_vm ${XIR_CHECKED_GENERATED}
    VERBATIM)
add_executable(test_xir_source_native xir/test_xir_source_native.c ${XIR_SOURCE_GENERATED})
target_link_libraries(test_xir_source_native PRIVATE xray_xir_scalar)
add_executable(test_xir_source_mixed xir/test_xir_source_mixed.c ${XIR_SOURCE_GENERATED})
target_link_libraries(test_xir_source_mixed PRIVATE xray_xir_source xray_xir_vm)
target_compile_definitions(test_xir_source_mixed PRIVATE XR_SOURCE_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_source")
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/enum_identity.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/enum_description.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/class_identity.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/class_hierarchy_contract.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/class_move_shape.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/class_stage_contract.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/class_dispatch_runtime.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/class_inheritance.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/class_inline.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/library.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/library_strings.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/intrinsic_registry.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/library_state.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/library_state_unit_bool.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/unit_destructure_cell.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/library_state_typed_values.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/assert_condition.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/assert_panics.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/assert_equal.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/enum_equal.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/nullable.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/runtime_sdk.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/stdlib_output_publication.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/host_call_lifecycle.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/time_services.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/default_parameters.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/default_invokes.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/class_array_fields.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/dependency_ready.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/initialization_failure.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/unit_slots.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/unit_slot_failure.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/generic_fields.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/nested_nullable.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/semantic60_transition.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/unit_locals.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/array_iteration.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/array_reorder_owner.cmake)
add_executable(test_xir_source_arrays xir/test_xir_source_arrays.c)
target_link_libraries(test_xir_source_arrays PRIVATE xray_xir_source xray_xir_vm)
target_compile_definitions(test_xir_source_arrays PRIVATE XR_ARRAY_SOURCE_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_array_source"
    XR_ARRAY_REORDER_SOURCE_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_array_reordering_source")
set(XIR_ARRAY_CHECKED_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_array_source.xrc)
set(XIR_ARRAY_BOUNDS_CHECKED_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_array_bounds.xrc)
set(XIR_ARRAY_REORDER_CHECKED_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_array_reorder.xrc)
add_custom_command(OUTPUT ${XIR_ARRAY_CHECKED_GENERATED} ${XIR_ARRAY_BOUNDS_CHECKED_GENERATED} ${XIR_ARRAY_REORDER_CHECKED_GENERATED}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND $<TARGET_FILE:test_xir_source_array_packets> ${XIR_ARRAY_CHECKED_GENERATED} ${XIR_ARRAY_BOUNDS_CHECKED_GENERATED} ${XIR_ARRAY_REORDER_CHECKED_GENERATED}
    DEPENDS test_xir_source_array_packets
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_array_source/root.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_array_source/producer.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_array_source/bounds.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_array_reordering_source/root.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_array_reordering_source/producer.xr
    VERBATIM)
add_executable(test_xir_array_packet_vm xir/test_xir_array_packet_vm.c ${XIR_ARRAY_CHECKED_GENERATED} ${XIR_ARRAY_BOUNDS_CHECKED_GENERATED} ${XIR_ARRAY_REORDER_CHECKED_GENERATED})
target_link_libraries(test_xir_array_packet_vm PRIVATE xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_xir_array_packet_vm PRIVATE XR_ARRAY_CHECKED_FIXTURE="${XIR_ARRAY_CHECKED_GENERATED}")
target_compile_definitions(test_xir_array_packet_vm PRIVATE XR_ARRAY_BOUNDS_CHECKED_FIXTURE="${XIR_ARRAY_BOUNDS_CHECKED_GENERATED}"
    XR_ARRAY_REORDER_CHECKED_FIXTURE="${XIR_ARRAY_REORDER_CHECKED_GENERATED}")
add_executable(test_xir_source_floats xir/test_xir_source_floats.c)
target_link_libraries(test_xir_source_floats PRIVATE xray_xir_source xray_xir_vm)
target_compile_definitions(test_xir_source_floats PRIVATE XR_FLOAT_SOURCE_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_float_source")
set(XIR_FLOAT_CHECKED_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_float_source.xrc)
set(XIR_FLOAT_SOURCE_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_float_source.c)
add_custom_command(OUTPUT ${XIR_FLOAT_CHECKED_GENERATED}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND $<TARGET_FILE:test_xir_source_floats> ${XIR_FLOAT_CHECKED_GENERATED}
    DEPENDS test_xir_source_floats
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_float_source/root.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_float_source/producer.xr
    VERBATIM)
add_executable(test_xir_float_packet_vm xir/test_xir_float_packet_vm.c ${XIR_FLOAT_CHECKED_GENERATED})
target_link_libraries(test_xir_float_packet_vm PRIVATE xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_xir_float_packet_vm PRIVATE XR_FLOAT_CHECKED_FIXTURE="${XIR_FLOAT_CHECKED_GENERATED}")
add_custom_command(OUTPUT ${XIR_FLOAT_SOURCE_GENERATED}
    COMMAND $<TARGET_FILE:test_xir_float_packet_vm> ${XIR_FLOAT_SOURCE_GENERATED}
    DEPENDS test_xir_float_packet_vm ${XIR_FLOAT_CHECKED_GENERATED}
    VERBATIM)
add_executable(test_xir_source_float_native xir/test_xir_source_float_native.c ${XIR_FLOAT_SOURCE_GENERATED})
target_link_libraries(test_xir_source_float_native PRIVATE xray_xir_scalar)
add_executable(test_xir_source_float_mixed xir/test_xir_source_float_mixed.c ${XIR_FLOAT_SOURCE_GENERATED})
target_link_libraries(test_xir_source_float_mixed PRIVATE xray_xir_vm)
target_compile_definitions(test_xir_source_float_mixed PRIVATE XR_FLOAT_CHECKED_FIXTURE="${XIR_FLOAT_CHECKED_GENERATED}")
add_executable(test_xir_cleanup_source xir/test_xir_cleanup_source.c)
target_link_libraries(test_xir_cleanup_source PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_xir_cleanup_source PRIVATE XR_CLEANUP_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_cleanup_source")
include(xir/mixed_lifecycle.cmake)
include(xir/mixed_terminals.cmake)
include(xir/mixed_observation.cmake)
include(xir/mixed_class_lifetime.cmake)
include(xir/mixed_library_counter.cmake)
include(xir/mixed_root_permissions.cmake)
include(xir/mixed_worker_controls.cmake)
include(xir/mixed_string_worker.cmake)
include(xir/mixed_diamond.cmake)
include(xir/mixed_worker_waits.cmake)
include(xir/mixed_deep_recursion.cmake)
if(MSVC)
    foreach(cleanup_host IN ITEMS test_xir_cleanup_source)
        target_compile_options(${cleanup_host} PRIVATE /O2 /Ob2 /Oy-)
    endforeach()
endif()
add_executable(test_xir_panic_source xir/test_xir_panic_source.c)
target_link_libraries(test_xir_panic_source PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_xir_panic_source PRIVATE XR_PANIC_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_panic_source")
set(XIR_PANIC_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_panic_source.c)
add_custom_command(OUTPUT ${XIR_PANIC_GENERATED}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND $<TARGET_FILE:test_xir_panic_source> ${XIR_PANIC_GENERATED}
    DEPENDS test_xir_panic_source
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_panic_source/root.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_panic_source/initialization.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_panic_source/faults.xr
    VERBATIM)
add_executable(test_xir_panic_native xir/test_xir_panic_native.c ${XIR_PANIC_GENERATED})
target_link_libraries(test_xir_panic_native PRIVATE xray_xir_scalar)
add_executable(test_xir_panic_mixed xir/test_xir_panic_mixed.c ${XIR_PANIC_GENERATED})
target_link_libraries(test_xir_panic_mixed PRIVATE xray_xir_vm)
foreach(xir_source_test IN ITEMS test_xir_source test_xir_source_mixed test_xir_source_allocations test_xir_source_admission)
    target_compile_definitions(${xir_source_test} PRIVATE XR_SOURCE_STDLIB="${CMAKE_SOURCE_DIR}/stdlib")
endforeach()
foreach(xir_test IN ITEMS test_xir_cleanup_source test_xir_cleanup_driver test_xir_locals test_xir_local_native test_xir_generics test_xir_checked test_xir_checked_allocations test_xir_execution test_xir_emit test_xir_native test_xir_calls
    test_xir_emit_calls test_xir_call_native test_xir_values test_xir_value_allocations
    test_xir_emit_strings test_xir_string_native test_xir_string_vm test_xir_instances
    test_xir_emit_program test_xir_program_vm test_xir_program_native
    test_xir_emit_output test_xir_output_vm test_xir_output_native test_xir_source test_xir_source_native
    test_xir_source_admission test_xir_source_allocations test_xir_source_mixed test_xir_packet_vm test_xir_source_generics test_xir_source_query
    test_xir_source_arrays test_xir_array_packet_vm
    test_xir_source_floats test_xir_float_packet_vm test_xir_source_float_native test_xir_source_float_mixed
    test_xir_panic_source test_xir_panic_native test_xir_panic_mixed test_xir_effects_native test_xir_effects_mixed)
    if(MSVC)
        target_compile_options(${xir_test} PRIVATE /W4 /WX)
    else()
        target_compile_options(${xir_test} PRIVATE -Wall -Wextra -Werror)
    endif()
    if(WIN32 AND xir_test STREQUAL "test_xir_source_allocations")
        add_test(NAME ${xir_test} COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/xir/run_source_allocation_shards.py"
            --executable $<TARGET_FILE:${xir_test}>
            --reports-root "${CMAKE_CURRENT_BINARY_DIR}/source-allocation-reports"
            --source-root "${CMAKE_SOURCE_DIR}")
        set_tests_properties(${xir_test} PROPERTIES TIMEOUT 600 PROCESSORS 8)
    else()
        add_test(NAME ${xir_test} COMMAND ${xir_test})
    endif()
    set_tests_properties(${xir_test} PROPERTIES LABELS "unit;xir;execution;ownership;abi")
endforeach()

include("${CMAKE_CURRENT_SOURCE_DIR}/xir/generic_normal.cmake")

if(UNIX)
    target_link_libraries(test_xir_execution PRIVATE m)
    target_link_libraries(test_xir_native PRIVATE m)
endif()

# Task 295 CoreSpec metadata is intentionally independent of xray_core and both
# executors. This target compiles only the generated registry projection.
add_executable(test_core_spec
    core/test_core_spec.c
    ${CMAKE_SOURCE_DIR}/src/core/xr_core_spec_gen.c
)
target_include_directories(test_core_spec PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_dependencies(test_core_spec gen-core-spec)
add_test(NAME test_core_spec COMMAND $<TARGET_FILE:test_core_spec>)
set_tests_properties(test_core_spec PROPERTIES
    LABELS "unit;core-spec;metadata;task-295"
)

# Compile the real constructors and writer with allocator substitution confined to this
# executable. Other consumers link the ordinary production objects.

# This fixture retains its existing legacy compiler object ownership.





add_xray_unit_test(test_xvalue value/test_xvalue.c)

# ============================================================================
# util/ — Hash, dynamic array, hashmap, arena
# ============================================================================

add_xray_unit_test(test_xhash util/test_xhash.c)
add_xray_unit_test(test_dynarray util/test_dynarray.c)
include("${CMAKE_CURRENT_LIST_DIR}/util/hashmap.cmake")
include(${CMAKE_CURRENT_SOURCE_DIR}/util/arena.cmake)

# ============================================================================
# mem/ — RC memory model
# ============================================================================

add_xray_unit_test(test_obj_header mem/test_obj_header.c)

# A root-level deep copy lands on the fixed heap, so the copied array must
# carry an initialised accounting owner or teardown dereferences garbage.

# The runtime half of `weak`, tested before any language
# surface exists for it (W1 read-promotes, W5 definite clearing point).

# The whole-heap traversal is validated against
# heap->object_count before any cycle logic runs on top of it. The body is
# compiled out unless XR_ENABLE_CYCLE_DETECTOR is on, so the test is a no-op
# in a default build rather than a missing target.

# The shared-domain scan (system-heap registry, dual-band trial
# deletion, channel/task edges) plus per-domain metering. Same no-op-by-default
# shape as the traversal test above.

# These contract tests build only the low-level runtime modules. The focused
# validation loop stays independent of the full compiler archive. Ownership
# audit instrumentation is intentionally absent from release product archives.

if(WIN32)
    set(XRAY_PROVIDER_TEST_HOST_SOURCES
        ${CMAKE_SOURCE_DIR}/src/os/win/time_win.c
        ${CMAKE_SOURCE_DIR}/src/os/win/random_win.c
        ${CMAKE_SOURCE_DIR}/src/os/win/pipe_win.c)
else()
    set(XRAY_PROVIDER_TEST_HOST_SOURCES
        ${CMAKE_SOURCE_DIR}/src/os/unix/time_unix.c
        ${CMAKE_SOURCE_DIR}/src/os/unix/random_unix.c
        ${CMAKE_SOURCE_DIR}/src/os/unix/pipe_unix.c)
endif()

add_executable(test_time_utc_offset runtime/test_time_utc_offset.c)
target_include_directories(test_time_utc_offset PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(NOT WIN32)
    target_compile_definitions(test_time_utc_offset PRIVATE _POSIX_C_SOURCE=200809L)

endif()
add_test(NAME test_time_utc_offset COMMAND $<TARGET_FILE:test_time_utc_offset>)
set_tests_properties(test_time_utc_offset PROPERTIES LABELS "runtime;provider;unit")
add_executable(test_windows_time_query os/test_windows_time_query.c)
target_include_directories(test_windows_time_query PRIVATE
    ${CMAKE_SOURCE_DIR}/src)
if(NOT WIN32)
    target_include_directories(test_windows_time_query PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/os/clock_api_fixture)
endif()
if(MSVC)
    target_compile_options(test_windows_time_query PRIVATE /W4 /WX)
else()
    target_compile_options(test_windows_time_query PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_windows_time_query COMMAND $<TARGET_FILE:test_windows_time_query>)
set_tests_properties(test_windows_time_query PROPERTIES LABELS "runtime;provider;unit;api-injection")
if(NOT WIN32)
    add_executable(test_time_query_failure os/test_time_query_failure.c)
    target_include_directories(test_time_query_failure PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(test_time_query_failure PRIVATE _POSIX_C_SOURCE=200809L)
    target_compile_options(test_time_query_failure PRIVATE -Wall -Wextra -Werror)
    add_test(NAME test_time_query_failure COMMAND $<TARGET_FILE:test_time_query_failure>)
    set_tests_properties(test_time_query_failure PROPERTIES LABELS "runtime;provider;unit")
endif()

# The shared typed text kernel is header-only and executor-independent, so its
# boundary and fault tests link nothing but the header.
add_executable(test_text_kernel
    runtime/test_text_kernel.c
)
target_include_directories(test_text_kernel PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
add_test(NAME test_text_kernel COMMAND $<TARGET_FILE:test_text_kernel>)
set_tests_properties(test_text_kernel PROPERTIES LABELS "runtime;string;canonical")

add_executable(test_byte_compare_core runtime/test_byte_compare_core.c)
target_include_directories(test_byte_compare_core PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(MSVC OR (CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    target_compile_options(test_byte_compare_core PRIVATE /W4 /WX)
else()
    target_compile_options(test_byte_compare_core PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_byte_compare_core COMMAND $<TARGET_FILE:test_byte_compare_core>)
set_tests_properties(test_byte_compare_core PROPERTIES LABELS "unit;crypto;runtime;kernel")

add_test(
    NAME check_runtime_object_header_boundary
    COMMAND ${XRAY_PYTHON}
        ${CMAKE_SOURCE_DIR}/scripts/check_runtime_object_header_boundary.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(check_runtime_object_header_boundary PROPERTIES
    LABELS "runtime;abi;static-analysis;target-machine")







# ============================================================================
# object/ — Runtime objects (string, array, utf8)
# ============================================================================

add_xray_unit_test(test_utf8_diagnostic base/test_utf8_diagnostic.c)
add_xray_unit_test(test_utf8 object/test_utf8.c)

# ============================================================================
# frontend/ — Lexer, parser, diagnostics
# ============================================================================

add_xray_unit_test(test_lexer frontend/test_lexer.c)
include("${CMAKE_CURRENT_LIST_DIR}/frontend/lexer_keywords.cmake")
add_xray_unit_test(test_lexer_positions frontend/test_lexer_positions.c)
add_xray_unit_test(test_lexer_trivia frontend/test_lexer_trivia.c)
add_executable(test_parser frontend/test_parser.c)
target_link_libraries(test_parser PRIVATE xray_compiler_parser)
add_test(NAME test_parser COMMAND $<TARGET_FILE:test_parser>)
add_xray_unit_test(test_parser_type_scope frontend/test_parser_type_scope.c)
add_executable(test_parser_recoverable frontend/test_parser_recoverable.c)
target_link_libraries(test_parser_recoverable PRIVATE xray_compiler_parser)
add_test(NAME test_parser_recoverable COMMAND $<TARGET_FILE:test_parser_recoverable>)
if(MSVC)
    target_compile_options(test_parser_recoverable PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_parser_recoverable PRIVATE -Wall -Wextra -Werror)
endif()
add_executable(test_parser_where_limits frontend/test_parser_where_limits.c)
target_link_libraries(test_parser_where_limits PRIVATE xray_compiler_parser)
add_test(NAME test_parser_where_limits COMMAND $<TARGET_FILE:test_parser_where_limits>)
set_tests_properties(test_parser_where_limits PROPERTIES LABELS "unit;parser;ownership;safety")
if(MSVC)
    target_compile_options(test_parser_where_limits PRIVATE /W4 /WX)
else()
    target_compile_options(test_parser_where_limits PRIVATE -Wall -Wextra -Werror)
endif()
add_executable(test_ast_allocation_failure frontend/test_ast_allocation_failure.c)
target_link_libraries(test_ast_allocation_failure PRIVATE xray_compiler_parser)
target_compile_features(test_ast_allocation_failure PRIVATE c_std_11)
if(MSVC)
    target_compile_definitions(test_ast_allocation_failure PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_options(test_ast_allocation_failure PRIVATE /W4 /WX /utf-8 /Gy)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_ast_allocation_failure PRIVATE /experimental:c11atomics)
    endif()
else()
    target_compile_options(test_ast_allocation_failure PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_ast_allocation_failure
    COMMAND ${CMAKE_COMMAND} -DTEST_EXECUTABLE=$<TARGET_FILE:test_ast_allocation_failure>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/frontend/check_ast_allocation_failure.cmake)
set_tests_properties(test_ast_allocation_failure PROPERTIES LABELS "unit;parser;ownership;safety" TIMEOUT 45)
add_executable(test_parser_asi frontend/test_parser_asi.c)
target_link_libraries(test_parser_asi PRIVATE xray_compiler_parser)
add_test(NAME test_parser_asi COMMAND $<TARGET_FILE:test_parser_asi>)
if(MSVC)
    target_compile_options(test_parser_asi PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_parser_asi PRIVATE -Wall -Wextra -Werror)
endif()
add_xray_unit_test(test_diag_fmt frontend/test_diag_fmt.c)
include(frontend/format_owner.cmake)
include(frontend/error_syntax.cmake)
include(cli/fmt_owner/CMakeLists.txt)

# Phase 3.5: X-01 AstNode -> XrType side table acceptance tests.

# Phase 3: in-tree frontend architecture lint. Walks the project src/
# tree at runtime, so it needs the absolute path baked in at build
# time. The test does NOT link xray_core (it is a pure path/text
# walker), so we add it directly without the helper macro.
add_executable(test_frontend_architecture
    frontend/test_frontend_architecture.c
)
target_include_directories(test_frontend_architecture PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_definitions(test_frontend_architecture PRIVATE
    XR_TEST_SRC_DIR="${CMAKE_SOURCE_DIR}/src"
)
add_test(NAME test_frontend_architecture
         COMMAND test_frontend_architecture)

# ============================================================================
# analyzer/ — Type inference, type checking
# ============================================================================

# ============================================================================
# vm/ — Instruction encoding, opcode
# ============================================================================

# xray_core already PUBLIC-links pthread (coroutines need it), so the
# concurrent IC test inherits the dependency through xray_core.

add_test(
    NAME test_grapheme_internal_boundary
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/vm/test_grapheme_internal_boundary.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(test_grapheme_internal_boundary PROPERTIES
    LABELS "unicode;vm;task-199"
)
add_test(
    NAME test_release_evidence_contract
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/release/test_release_evidence.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(test_release_evidence_contract PROPERTIES
    LABELS "install;release;task-240"
)

# ============================================================================
# symbol/ — Symbol table
# ============================================================================

# ============================================================================
# ir/ — New typed SSA IR (Xi)
# ============================================================================



function(xr_enable_pure_aot_symbol_map target_name)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC"
            OR (CMAKE_C_COMPILER_ID STREQUAL "Clang"
                AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
        target_link_options(${target_name} PRIVATE
            "/MAP:${CMAKE_CURRENT_BINARY_DIR}/${target_name}.map")
    elseif(WIN32)
        target_link_options(${target_name} PRIVATE
            "LINKER:/MAP:${CMAKE_CURRENT_BINARY_DIR}/${target_name}.map")
    endif()
endfunction()
include("${CMAKE_CURRENT_SOURCE_DIR}/xir/library_views_native.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/xir/library_typed_native.cmake")

math(EXPR XR_PROGRAM_NATIVE_POINTER_BITS "${CMAKE_SIZEOF_VOID_P} * 8")





set_source_files_properties(${XR_PROGRAM_POINTER_AOT_GENERATED_C}
    PROPERTIES GENERATED TRUE)





set_source_files_properties(${XR_PROGRAM_YIELD_AOT_GENERATED_C}
    PROPERTIES GENERATED TRUE)





set_source_files_properties(${XR_PROGRAM_COROUTINE_CALL_AOT_GENERATED_C}
    PROPERTIES GENERATED TRUE)






# Each manifest fixture has one producer and one independent native qualification.
# An empty expected_stdout_hex only checks the exit byte; otherwise the native
# process must write exactly those bytes to stdout.

set(XR_PROGRAM_IMPORTED_CALLABLE_FIXTURE_DIR
    ${CMAKE_CURRENT_BINARY_DIR}/generated/task-301-imported-callable)
set(XR_PROGRAM_IMPORTED_CALLABLE_GENERATED_C
    ${XR_PROGRAM_IMPORTED_CALLABLE_FIXTURE_DIR}/imported_callable_native.c)

set_source_files_properties(${XR_PROGRAM_IMPORTED_CALLABLE_GENERATED_C}
    PROPERTIES GENERATED TRUE)
add_executable(test_xr_program_imported_callable_native
    ${XR_PROGRAM_IMPORTED_CALLABLE_GENERATED_C})

if(CMAKE_C_COMPILER_ID STREQUAL "MSVC"
        OR (CMAKE_C_COMPILER_ID STREQUAL "Clang"
            AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    target_compile_options(test_xr_program_imported_callable_native PRIVATE /W4 /WX)
else()
    target_compile_options(test_xr_program_imported_callable_native PRIVATE
        -std=c11 -pedantic-errors -Wall -Wextra -Werror)
endif()
add_test(NAME test_xr_program_imported_callable_native
    COMMAND ${XRAY_PYTHON}
        ${CMAKE_SOURCE_DIR}/scripts/check_xr_program_aot_native.py
        --executable $<TARGET_FILE:test_xr_program_imported_callable_native>
        --expected-exit 42)
set_tests_properties(test_xr_program_imported_callable_native PROPERTIES
    LABELS "unit;aot;canonical-program;source;callable;multi-module;generated-c;native;task-301")

set(XR_OPTIONAL_STORAGE_SOURCE_PREFIX
    ${CMAKE_CURRENT_BINARY_DIR}/generated/optional_storage)

foreach(XR_OPTIONAL_SCENARIO RANGE 0 8)
    set(XR_OPTIONAL_NATIVE_TARGET test_xi_optional_native_${XR_OPTIONAL_SCENARIO})
    set(XR_OPTIONAL_NATIVE_SOURCE ${XR_OPTIONAL_STORAGE_SOURCE_PREFIX}.${XR_OPTIONAL_SCENARIO}.c)
    set_source_files_properties(${XR_OPTIONAL_NATIVE_SOURCE}
        PROPERTIES GENERATED TRUE HEADER_FILE_ONLY TRUE)
    add_executable(${XR_OPTIONAL_NATIVE_TARGET}
        aot/xi_optional_storage_native.c ${XR_OPTIONAL_NATIVE_SOURCE})
    target_compile_definitions(${XR_OPTIONAL_NATIVE_TARGET} PRIVATE
        XR_OPTIONAL_SCENARIO=${XR_OPTIONAL_SCENARIO}
        XR_OPTIONAL_GENERATED_C="${XR_OPTIONAL_NATIVE_SOURCE}")

    target_include_directories(${XR_OPTIONAL_NATIVE_TARGET} SYSTEM PRIVATE
        ${CMAKE_SOURCE_DIR}/src/aot ${CMAKE_SOURCE_DIR}/include)
    set_target_properties(${XR_OPTIONAL_NATIVE_TARGET} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED TRUE C_EXTENSIONS FALSE)
    if(MSVC)
        target_compile_options(${XR_OPTIONAL_NATIVE_TARGET} PRIVATE /W4 /WX /wd4702)
    else()
        target_compile_options(${XR_OPTIONAL_NATIVE_TARGET} PRIVATE -Wall -Wextra -Werror -pedantic-errors)
    endif()
    add_test(NAME ${XR_OPTIONAL_NATIVE_TARGET} COMMAND $<TARGET_FILE:${XR_OPTIONAL_NATIVE_TARGET}>)
    set_tests_properties(${XR_OPTIONAL_NATIVE_TARGET} PROPERTIES LABELS "aot;cgen;optional;native")
endforeach()
set(XR_NATIVE_EXPECTED_structural_root 34320a330a2d310a31370a39390a34320a390a2f726f6f743f7123660a)
set(XR_NATIVE_EXPECTED_runtime_constructor 520a3d0a34320a747275650a322e350a)
set(XR_NATIVE_EXPECTED_coroutine_aggregate 626574610a34320a)
set(XR_NATIVE_EXPECTED_returned_class 34320a)
set(XR_NATIVE_EXPECTED_byte_bitwise 3235350a300a3235350a)
set(XR_NATIVE_EXPECTED_default_bool_not 66616c73650a747275650a66616c73650a)
set(XR_NATIVE_EXPECTED_nullable_field 656d7074790a6b6570740a)
set(XR_NATIVE_EXPECTED_filled_array 31300a)
set(XR_NATIVE_EXPECTED_optional_method 34320a)
set(XR_NATIVE_EXPECTED_slice_copy 35300a)
set(XR_NATIVE_EXPECTED_method_ref_array 34320a)
set(XR_NATIVE_EXPECTED_sync_go 32350a330a320a320a)
set(XR_NATIVE_EXPECTED_raw_slice 320a)
set(XR_NATIVE_EXPECTED_optional_injection 747275650a66616c73650a2d370a747275650a66616c73650a747275650a)
set(XR_NATIVE_EXPECTED_unsigned_text 76616c75653d300a76616c75653d31383434363734343037333730393535313631350a)
set(XR_NATIVE_EXPECTED_encoding_slice 48616c6c6f2048616c6c6f0a35203130340a747275652035203130340a747275652074727565207472756520747275650af09f998242200a6c6c6f0a350a6361756768740a)
set(XR_NATIVE_EXPECTED_native_array 747275650a66616c73650a747275650a310a)
set(XR_NATIVE_EXPECTED_structural_array "")
set(XR_NATIVE_EXPECTED_rune_values 4120f09f9982204120f09f99820a6865783a610a7472756520747275652074727565207472756520747275650a)
set(XR_NATIVE_EXPECTED_module_ref 360a34300a390a)
set(XR_NATIVE_EXPECTED_entry_closure 6c6962726172792072656164790a34320a372034320a39390a)
set(XR_NATIVE_EXPECTED_native_storage 31300a)
set(XR_NATIVE_EXPECTED_module_struct_copy 36380a36380a31203220330a)
set(XR_NATIVE_EXPECTED_explicit_concat 61316232747275656e756c6c4b696e642e52656164790a71330a34320a)
foreach(XR_NATIVE_FIXTURE structural_root runtime_constructor coroutine_aggregate returned_class nullable_field default_bool_not byte_bitwise filled_array optional_method slice_copy method_ref_array sync_go raw_slice optional_injection unsigned_text encoding_slice native_array structural_array rune_values module_ref entry_closure native_storage module_struct_copy explicit_concat)
    string(REPLACE "_" "-" XR_NATIVE_SELECTOR ${XR_NATIVE_FIXTURE})
    if(XR_NATIVE_FIXTURE STREQUAL "optional_injection")
        set(XR_NATIVE_SELECTOR optional-injection-emission)
    endif()
    set(XR_NATIVE_GENERATED_C ${CMAKE_CURRENT_BINARY_DIR}/generated/${XR_NATIVE_FIXTURE}.c)
    set(XR_NATIVE_TARGET test_xi_${XR_NATIVE_FIXTURE}_native)

    if(XR_NATIVE_FIXTURE MATCHES "^(module_ref|entry_closure|native_storage)$")
        set(XR_NATIVE_TARGET test_xaot_${XR_NATIVE_FIXTURE}_native)

    endif()
    add_custom_command(
        OUTPUT ${XR_NATIVE_GENERATED_C}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
        COMMAND ${XR_NATIVE_GENERATE_COMMAND}
        DEPENDS ${XR_NATIVE_GENERATOR}
        COMMENT "Generating verified ${XR_NATIVE_FIXTURE} C fixture"
        VERBATIM)
    set_source_files_properties(${XR_NATIVE_GENERATED_C} PROPERTIES GENERATED TRUE)
    add_executable(${XR_NATIVE_TARGET} ${XR_NATIVE_GENERATED_C})

    target_include_directories(${XR_NATIVE_TARGET} SYSTEM PRIVATE
        ${CMAKE_SOURCE_DIR}/src/aot ${CMAKE_SOURCE_DIR}/include)
    set_target_properties(${XR_NATIVE_TARGET} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED TRUE C_EXTENSIONS FALSE)

    if(MSVC)
        target_compile_options(${XR_NATIVE_TARGET} PRIVATE /W4 /WX /wd4702)
    else()
        target_compile_options(${XR_NATIVE_TARGET} PRIVATE -Wall -Wextra -Werror -pedantic-errors)
    endif()
    add_test(NAME ${XR_NATIVE_TARGET}
        COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/scripts/check_xr_program_aot_native.py
            --executable $<TARGET_FILE:${XR_NATIVE_TARGET}>
            --expected-exit 0 --expected-stdout-hex "${XR_NATIVE_EXPECTED_${XR_NATIVE_FIXTURE}}")
    set_tests_properties(${XR_NATIVE_TARGET} PROPERTIES LABELS "aot;cgen;ownership;native")
endforeach()

set(XR_NATIVE_SOURCE_HOST_ARGS --host-compiler ${CMAKE_C_COMPILER})
if(MSVC)
    list(APPEND XR_NATIVE_SOURCE_HOST_ARGS --msvc)
endif()
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    list(APPEND XR_NATIVE_SOURCE_HOST_ARGS --msvc-atomics)
endif()
add_test(NAME test_native_string_search_authority
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/cli/test_native_string_search.py
        --compiler $<TARGET_FILE:test_source_generated_c> ${XR_NATIVE_SOURCE_HOST_ARGS})
set_tests_properties(test_native_string_search_authority PROPERTIES
    LABELS "aot;generated-c;runtime-method;integration"
    TIMEOUT 240)

add_test(NAME test_native_array_pop_authority
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/cli/test_native_array_pop.py
        --compiler $<TARGET_FILE:test_source_generated_c> ${XR_NATIVE_SOURCE_HOST_ARGS})
set_tests_properties(test_native_array_pop_authority PROPERTIES
    LABELS "aot;generated-c;array;ownership;integration" TIMEOUT 240)
add_test(NAME test_native_tuple_boundary_authority
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/cli/test_native_tuple_boundary.py
        --compiler $<TARGET_FILE:test_source_generated_c> ${XR_NATIVE_SOURCE_HOST_ARGS})
set_tests_properties(test_native_tuple_boundary_authority PROPERTIES
    LABELS "aot;generated-c;tuple;ownership;integration" TIMEOUT 240)
add_test(NAME test_native_string_concat_authority
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/cli/test_native_string_concat.py
        --compiler $<TARGET_FILE:test_source_generated_c> ${XR_NATIVE_SOURCE_HOST_ARGS})
set_tests_properties(test_native_string_concat_authority PROPERTIES
    LABELS "aot;generated-c;string;ownership;integration" TIMEOUT 120)

add_test(NAME test_enum_namespace_execution
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/cli/run_enum_namespace_tests.py
        --compiler $<TARGET_FILE:xray>)
set_tests_properties(test_enum_namespace_execution PROPERTIES
    LABELS "vm;aot;enum;integration" TIMEOUT 300)


# Complete programs lowered by test_xi_cgen, strictly compiled, linked with the
# same AOT core archive the CLI links, executed, and compared byte for byte
# with independent answers kept in the runner.
set(XR_CGEN_EXECUTION_HOST_ARGS ${XR_NATIVE_SOURCE_HOST_ARGS})
if(ENABLE_ASAN OR ENABLE_SANITIZERS)
    list(APPEND XR_CGEN_EXECUTION_HOST_ARGS --asan)
endif()
if(ENABLE_UBSAN OR ENABLE_SANITIZERS)
    list(APPEND XR_CGEN_EXECUTION_HOST_ARGS --ubsan)
endif()


add_test(NAME test_native_class_coalesce_authority
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/cli/test_native_class_coalesce.py
        --compiler $<TARGET_FILE:test_source_generated_c> ${XR_NATIVE_SOURCE_HOST_ARGS})
set_tests_properties(test_native_class_coalesce_authority PROPERTIES
    LABELS "aot;generated-c;class;ownership;integration" TIMEOUT 240)
add_test(NAME test_native_ref_array_authority
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/cli/test_native_ref_array.py
        --compiler $<TARGET_FILE:test_source_generated_c> ${XR_NATIVE_SOURCE_HOST_ARGS})
set_tests_properties(test_native_ref_array_authority PROPERTIES
    LABELS "aot;generated-c;array;ownership;integration" TIMEOUT 240)
add_test(NAME test_native_nullable_enum_authority
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/cli/test_native_nullable_enum.py
        --compiler $<TARGET_FILE:test_source_generated_c> ${XR_NATIVE_SOURCE_HOST_ARGS})
set_tests_properties(test_native_nullable_enum_authority PROPERTIES
    LABELS "aot;generated-c;enum;integration" TIMEOUT 240)
add_test(NAME test_native_module_const_authority
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/cli/test_native_module_const.py
        --compiler $<TARGET_FILE:test_source_generated_c> ${XR_NATIVE_SOURCE_HOST_ARGS})
set_tests_properties(test_native_module_const_authority PROPERTIES
    LABELS "aot;generated-c;array;ownership;integration" TIMEOUT 240)

add_test(NAME test_native_atomic_authority
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/cli/test_native_atomic.py
        --compiler $<TARGET_FILE:test_source_generated_c> ${XR_NATIVE_SOURCE_HOST_ARGS})
set_tests_properties(test_native_atomic_authority PROPERTIES
    LABELS "aot;generated-c;atomic;ownership;integration" TIMEOUT 240)

set(XR_LEAF_PRODUCT_NATIVE_SOURCE
    ${CMAKE_CURRENT_BINARY_DIR}/generated/leaf_product_native.c)

set_source_files_properties(${XR_LEAF_PRODUCT_NATIVE_SOURCE}
    PROPERTIES GENERATED TRUE)
add_executable(test_leaf_product_native
    ${XR_LEAF_PRODUCT_NATIVE_SOURCE})
set_target_properties(test_leaf_product_native PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED TRUE)
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC"
        OR (CMAKE_C_COMPILER_ID STREQUAL "Clang"
            AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    target_compile_options(test_leaf_product_native PRIVATE /W4 /WX)
else()
    target_compile_options(test_leaf_product_native PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_leaf_product_native
    COMMAND $<TARGET_FILE:test_leaf_product_native>)
set_tests_properties(test_leaf_product_native PROPERTIES
    LABELS "aot;cgen;native;target-machine;task-281")

# This opt-in tool refreshes the checked-in runtime fixture when the canonical
# artifact schema changes. It is intentionally excluded from normal builds and
# is not a dependency of the runtime-only consumer below.

# The manifest keeps this gate in exact expected-unsupported mode until both
# backend owners explicitly activate their probe protocol.  Once active, the
# same CTest invokes these existing executables and compares all four executor
# records with the frozen Reference oracle. The AOT generated-C mode owns a
# real strict-C11 compile and host-run; emitting or scanning C alone cannot
# produce a qualifying record.

add_test(NAME xr_program_allocation_native_self_test
    COMMAND ${XRAY_PYTHON} -m unittest discover
        -s ${CMAKE_SOURCE_DIR}/tests/lib/tests -p test_program_allocation_native.py)
set_tests_properties(xr_program_allocation_native_self_test PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    LABELS "unit;aot;generated-c;allocation;self-test")

add_test(NAME xr_program_coroutine_graph_native_self_test
    COMMAND ${XRAY_PYTHON} -m unittest discover
        -s ${CMAKE_SOURCE_DIR}/tests/lib/tests -p test_program_coroutine_graph_native.py)
set_tests_properties(xr_program_coroutine_graph_native_self_test PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    LABELS "unit;aot;generated-c;coroutine;self-test")

if(WIN32)
    add_test(NAME test_xr_program_aot_real_artifact COMMAND ${XRAY_PYTHON} -X utf8
        ${CMAKE_CURRENT_SOURCE_DIR}/cli/native_admission_owner/test_native_admission.py
        $<TARGET_FILE:test_native_admission_production> ${PROJECT_SOURCE_DIR}
        "${ADMISSION_MSVC}" "${ADMISSION_LINKER}" "${ADMISSION_SDK}" native)
    set_tests_properties(test_xr_program_aot_real_artifact PROPERTIES
        LABELS "unit;aot;native-artifact;identity;xir" TIMEOUT 300 RUN_SERIAL TRUE)
endif()

include(xir/atomic_native20.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/array_generated.cmake)

include(${CMAKE_CURRENT_SOURCE_DIR}/xir/rune_text_output.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/fixed_bytes.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/source_copy.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/array_binding.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/nominal_inference.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/tuple_assignment.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/array_materialization.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/optional_chain.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/mutslice_index.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/buffer_scoped_boundary.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/crypto_native.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/stringable.cmake)
include(${PROJECT_SOURCE_DIR}/tests/cli/csv_errors.cmake)

include("xir/module_state_zero.cmake")
include("xir/module_state_one.cmake")
include("xir/module_state_failures.cmake")
include("xir/module_state_classes.cmake")
include("xir/module_state_remaining.cmake")

# The bounded program capability needs an arbitrarily ordered XSM module set
# plus one exact XTP, which no single-module fixture can stand in for. The
# writer is a CLI mode of the closure test rather than one of its cases, so it
# produces the artifacts without running them.








set(XR_XTP_FUZZ_EVIDENCE_SANITIZER release)
if(ENABLE_SANITIZERS OR (ENABLE_ASAN AND ENABLE_UBSAN))
    set(XR_XTP_FUZZ_EVIDENCE_SANITIZER asan-ubsan)
elseif(ENABLE_TSAN)
    set(XR_XTP_FUZZ_EVIDENCE_SANITIZER tsan)
endif()
add_test(
    NAME test_xtp_fuzz_evidence_runner
    COMMAND ${XRAY_PYTHON}
        ${CMAKE_SOURCE_DIR}/tests/fuzz/test_run_xtp_fuzz_evidence.py
)
set_tests_properties(test_xtp_fuzz_evidence_runner PROPERTIES
    LABELS "target-machine;vm;typed-plan;loader;fuzz"
    PROCESSORS 1
    TIMEOUT 30)

add_xray_unit_test(test_fs_atomic os/test_fs_atomic.c)

add_executable(test_cgen_verify_output aot/test_cgen_verify_output.c)
target_include_directories(test_cgen_verify_output PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(test_cgen_verify_output PRIVATE xray_xir_cgen)
if(MSVC)
    target_compile_options(test_cgen_verify_output PRIVATE /W4 /WX /utf-8)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        target_compile_options(test_cgen_verify_output PRIVATE /experimental:c11atomics)
    endif()
else()
    target_compile_options(test_cgen_verify_output PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_cgen_verify_output COMMAND test_cgen_verify_output)
set_tests_properties(test_cgen_verify_output PROPERTIES LABELS "aot;cgen;task-218")
add_executable(test_cgen_verify_allocations aot/test_cgen_verify_allocations.c)
target_include_directories(test_cgen_verify_allocations PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(test_cgen_verify_allocations PRIVATE xray_xir_cgen)
if(MSVC)
    target_compile_options(test_cgen_verify_allocations PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_cgen_verify_allocations PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_cgen_verify_allocations COMMAND ${XRAY_PYTHON}
    ${CMAKE_CURRENT_SOURCE_DIR}/aot/test_cgen_verify_allocations.py $<TARGET_FILE:test_cgen_verify_allocations>)
set_tests_properties(test_cgen_verify_allocations PROPERTIES LABELS "unit;aot;cgen;ownership;task-218")

# This case compiles the header-only runtime rather than linking xray_core, so
# it does not inherit the math library the core links for everyone else. Darwin
# serves math from libSystem and never noticed; a glibc host fails to resolve
# nan() at link time.

add_test(NAME test_assertion_vm_equality
    COMMAND $<TARGET_FILE:xray> run
            ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/assertion/deep_equality_vm.xr)
set_tests_properties(test_assertion_vm_equality PROPERTIES
    LABELS "vm;assertion;shared-core")
add_test(NAME test_assertion_retired_names_are_ordinary
    COMMAND $<TARGET_FILE:xray> run
            ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/assertion/retired_names_are_ordinary.xr)
set_tests_properties(test_assertion_retired_names_are_ordinary PROPERTIES
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    LABELS "frontend;vm;assertion;removed-surface")
add_test(NAME test_assertion_same_t_contextual
    COMMAND $<TARGET_FILE:xray> run
            ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/assertion/same_t_contextual.xr)
set_tests_properties(test_assertion_same_t_contextual PROPERTIES
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    LABELS "frontend;vm;assertion;type-system")
add_test(NAME test_array_growth_accounting
    COMMAND $<TARGET_FILE:xray> run
            ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/runtime/array_growth_accounting.xr)
set_tests_properties(test_array_growth_accounting PROPERTIES
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    LABELS "vm;runtime;memory")

# This test deliberately compiles the header-only AOT runtime with XRT_IMPL and
# custom allocators.  Linking xray_core would give it a second runtime-global
# owner and fails deterministically under sanitizer linkers.

# Generated AOT cores are also compiled as C++ by governed compatibility
# workflows.  Keep this target at the minimum supported C++ dialect and avoid
# linking xray_core so the header-only ABI/atomic contract is tested directly.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR
   CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR
   CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    add_executable(test_xr_atomic_cxx aot/test_xr_atomic_cxx.cpp)
    target_include_directories(test_xr_atomic_cxx PRIVATE ${XRAY_COMMON_INCLUDES})
    set_target_properties(test_xr_atomic_cxx PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON)
    add_test(NAME test_xr_atomic_cxx COMMAND $<TARGET_FILE:test_xr_atomic_cxx>)
    set_tests_properties(test_xr_atomic_cxx PROPERTIES LABELS "aot;cgen;cxx;task-243")
endif()

# ============================================================================
# base/ — Config, logging, SIMD, SWAR, mutex, intmap, mem_profiler
# ============================================================================

add_xray_unit_test(test_config base/test_config.c)
add_xray_unit_test(test_log base/test_log.c)
add_xray_unit_test(test_swar base/test_swar.c)
add_xray_unit_test(test_hash_core base/test_hash_core.c)
add_xray_unit_test(test_cstr_core base/test_cstr_core.c)
add_xray_unit_test(test_error_core base/test_error_core.c)
add_xray_unit_test(test_builtin_schema base/test_builtin_schema.c)
add_xray_unit_test(test_map_set_abi base/test_map_set_abi.c)
add_xray_unit_test(test_native_type_core base/test_native_type_core.c)
# This allocation-free header has no compiler, stdlib or runtime owner.
add_executable(test_value_format_core base/test_value_format_core.c)
target_include_directories(test_value_format_core PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_value_format_core COMMAND test_value_format_core)
add_xray_unit_test(test_swiss_index base/test_swiss_index.c)
add_xray_unit_test(test_typed_ops base/test_typed_ops.c)
add_xray_unit_test(test_simd base/test_simd.c)
add_xray_unit_test(test_mutex base/test_mutex.c)
add_xray_unit_test(test_intmap base/test_intmap.c)
add_xray_unit_test(test_mem_profiler base/test_mem_profiler.c)
add_xray_unit_test(test_xmalloc base/test_xmalloc.c)

# ============================================================================
# coro/ — MPSC queue, work-stealing queue
# ============================================================================

add_executable(test_channel_buffer runtime/test_channel_buffer.c)
target_include_directories(test_channel_buffer PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_channel_buffer COMMAND $<TARGET_FILE:test_channel_buffer>)
set_tests_properties(test_channel_buffer PROPERTIES LABELS "unit;runtime;channel;shared-core")

# This scheduler fixture consumes runtime state, not stage-2 stdlib overlays.

# ============================================================================
# module/ — Resolver, semver, lockfile, source cache
# ============================================================================

add_xray_unit_test(test_resolver module/test_resolver.c)
if(NOT WIN32)
  add_xray_unit_test(test_module_resolver module/test_module_resolver.c)
endif()
add_xray_unit_test(test_module_graph module/test_module_graph.c)

add_xray_unit_test(test_semver module/test_semver.c)
add_xray_unit_test(test_lockfile module/test_lockfile.c)
add_xray_unit_test(test_source_cache module/test_source_cache.c)
add_xray_unit_test(test_project module/test_project.c)

# ============================================================================
# encoding/ — Base64, hex, URL encoding, Unicode
# ============================================================================

add_xray_unit_test(test_unicode encoding/test_unicode.c)
add_xray_unit_test(test_unicode_grapheme_table encoding/test_unicode_grapheme_table.c)
add_xray_unit_test(test_unicode_grapheme_cursor encoding/test_unicode_grapheme_cursor.c)
target_compile_definitions(test_unicode_grapheme_cursor PRIVATE
    XR_GRAPHEME_BREAK_TEST_FILE="${CMAKE_SOURCE_DIR}/third_party/unicode/17.0.0/GraphemeBreakTest.txt"
)
add_test(
    NAME test_unicode_grapheme_generator
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/encoding/test_unicode_grapheme_generator.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(test_unicode_grapheme_generator PROPERTIES
    LABELS "unicode;generator;task-199"
)

# ============================================================================
# http/ — HTTP/2 helpers
# ============================================================================

add_xray_unit_test(test_http2_huffman http/test_http2_huffman.c)
# The HPACK Huffman table now lives in Xray. This test parses the six
# HUFFMAN_* constant arrays out of the .xr source at run time, so hand it an
# absolute path instead of depending on the test's working directory.
target_compile_definitions(test_http2_huffman PRIVATE
    XRAY_HTTP2_XR_SOURCE="${CMAKE_SOURCE_DIR}/stdlib/http2/http2.xr"
)

# ============================================================================
# stdlib/ — DateTime, crypto, compress, XML, buffer pool
# ============================================================================

add_xray_unit_test(test_array_core stdlib/test_array_core.c)
add_xray_unit_test(test_byte_array_copy_core stdlib/test_byte_array_copy_core.c)
add_xray_unit_test(test_byte_array_repeat_core stdlib/test_byte_array_repeat_core.c)
add_executable(test_byte_array_append_core stdlib/test_byte_array_append_core.c)
target_include_directories(test_byte_array_append_core PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
)
add_test(NAME test_byte_array_append_core COMMAND $<TARGET_FILE:test_byte_array_append_core>)
add_xray_unit_test(test_math_core stdlib/test_math_core.c)
add_xray_unit_test(test_numeric_core stdlib/test_numeric_core.c)
if(MSVC)
    target_compile_options(test_math_core PRIVATE /W4 /WX)
    target_compile_options(test_numeric_core PRIVATE /W4 /WX)
endif()
add_xray_unit_test(test_null_test_core stdlib/test_null_test_core.c)
add_xray_unit_test(test_compare_core stdlib/test_compare_core.c)

add_xray_unit_test(test_slice_window_core stdlib/test_slice_window_core.c)

add_xray_unit_test(test_range_core stdlib/test_range_core.c)
add_executable(test_data_pointer_core stdlib/test_data_pointer_core.c)
target_include_directories(test_data_pointer_core PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_CURRENT_SOURCE_DIR}
)
add_test(NAME test_data_pointer_core COMMAND $<TARGET_FILE:test_data_pointer_core>)

# The C90 half of the same family: it proves that the restricted data-pointer
# adapter compiles and runs under strict ISO C90. Its null_test and
# byte_array_copy siblings are registered below.
#
# Not add_xray_unit_test(): that macro links xray_core and does not set the
# standard, and the whole value of a C90 KAT is compiling without either.

add_executable(test_cell_access_core stdlib/test_cell_access_core.c)
target_include_directories(test_cell_access_core PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)
add_test(NAME test_cell_access_core COMMAND $<TARGET_FILE:test_cell_access_core>)

add_executable(test_enum_metadata_core stdlib/test_enum_metadata_core.c)
target_include_directories(test_enum_metadata_core PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_CURRENT_SOURCE_DIR}
)
add_test(NAME test_enum_metadata_core COMMAND $<TARGET_FILE:test_enum_metadata_core>)

add_xray_unit_test(test_string_core stdlib/test_string_core.c)
include(${CMAKE_CURRENT_SOURCE_DIR}/stdlib/string_core.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/base/decimal_rounding.cmake)
add_xray_unit_test(test_strbuf_core stdlib/test_strbuf_core.c)
add_xray_unit_test(test_truthy_core stdlib/test_truthy_core.c)
add_xray_unit_test(test_type_identity_core stdlib/test_type_identity_core.c)
add_xray_unit_test(test_io_core stdlib/test_io_core.c)
add_xray_unit_test(test_os_core stdlib/test_os_core.c)
add_executable(test_os_system_queries base/test_system_queries.c)
target_include_directories(test_os_system_queries PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(test_os_system_queries PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_os_system_queries PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_os_system_queries PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
if(NOT WIN32)
    target_compile_definitions(test_os_system_queries PRIVATE _POSIX_C_SOURCE=200809L _GNU_SOURCE)
endif()
add_test(NAME os_system_queries COMMAND test_os_system_queries)
set_tests_properties(os_system_queries PROPERTIES RUN_SERIAL TRUE TIMEOUT 60 LABELS "unit;ownership;os;system")
add_executable(test_os_file_metadata base/test_file_metadata.c)
target_include_directories(test_os_file_metadata PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(test_os_file_metadata PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_os_file_metadata PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_os_file_metadata PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
if(NOT WIN32)
    target_compile_definitions(test_os_file_metadata PRIVATE _POSIX_C_SOURCE=200809L _GNU_SOURCE)
endif()
add_test(NAME os_file_metadata COMMAND test_os_file_metadata)
set_tests_properties(os_file_metadata PROPERTIES RUN_SERIAL TRUE TIMEOUT 60 LABELS "unit;ownership;os;filesystem")
add_executable(test_os_file_mutation base/test_file_mutation.c)
target_include_directories(test_os_file_mutation PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(test_os_file_mutation PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_os_file_mutation PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_os_file_mutation PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
if(NOT WIN32)
    target_compile_definitions(test_os_file_mutation PRIVATE _POSIX_C_SOURCE=200809L _GNU_SOURCE)
endif()
add_test(NAME os_file_mutation COMMAND test_os_file_mutation)
set_tests_properties(os_file_mutation PROPERTIES RUN_SERIAL TRUE TIMEOUT 60 LABELS "unit;ownership;os;filesystem")
include("${CMAKE_CURRENT_LIST_DIR}/base/file_links.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/base/file_temp.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/base/directory_owned.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/base/pipe_owned.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/base/thread_token.cmake")
add_executable(test_os_signal base/test_signal.c)
target_include_directories(test_os_signal PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(test_os_signal PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_os_signal PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_os_signal PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
if(NOT WIN32)
    target_compile_definitions(test_os_signal PRIVATE _POSIX_C_SOURCE=200809L _GNU_SOURCE)
endif()
add_test(NAME os_signal COMMAND test_os_signal)
set_tests_properties(os_signal PROPERTIES RUN_SERIAL TRUE TIMEOUT 60 LABELS "unit;ownership;os;signal")
if(NOT WIN32)
    target_link_libraries(test_os_signal PRIVATE pthread)
endif()
add_xray_unit_test(test_crypto stdlib/test_crypto.c)
if(XR_STDLIB_COMPRESS)
    add_xray_unit_test(test_compress stdlib/test_compress.c)
endif()
add_xray_unit_test(test_xml_node stdlib/test_xml_node.c)

add_test(NAME test_native_declaration_schema
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/unit/frontend/test_native_declaration_schema.py)
set_tests_properties(test_native_declaration_schema PROPERTIES LABELS "stdlib;metadata;native-types")
add_test(NAME test_regression_corpus_ratchet
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/unit/frontend/test_regression_corpus_ratchet.py)
set_tests_properties(test_regression_corpus_ratchet PROPERTIES LABELS "unit;frontend;regression" TIMEOUT 30)
# The shared value ABI remains independent of stdlib implementation choices.
add_executable(test_hosted_value_abi stdlib/test_hosted_value_abi.c)
target_include_directories(test_hosted_value_abi PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME test_hosted_value_abi COMMAND test_hosted_value_abi)
set_tests_properties(test_hosted_value_abi PROPERTIES LABELS "unit;abi")
add_test(
    NAME test_stdlib_generators_content_stable
    COMMAND ${XRAY_PYTHON}
            ${CMAKE_CURRENT_SOURCE_DIR}/stdlib/test_stdlib_generators_content_stable.py
            --cc ${CMAKE_C_COMPILER}
)
set_tests_properties(test_stdlib_generators_content_stable PROPERTIES
    LABELS "stdlib;metadata;tooling;incremental-build"
)
# ============================================================================
# regex/ — Regex engine
# ============================================================================

# test_regex is gone with the C regex engine: it reached into stdlib/regex's
# internal headers, and the automata now live in stdlib/regex/regex.xr. Its
# coverage moved to tests/regression/10_stdlib/1101_regex_engine.xr.

# ============================================================================
# api/ — Public API defense tests
# ============================================================================

add_executable(test_api_defense api/test_api_defense.c)
target_link_libraries(test_api_defense PRIVATE xray_source_api)
target_include_directories(test_api_defense PRIVATE "${PROJECT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(test_api_defense PRIVATE XR_API_DEFENSE_STDLIB="${PROJECT_SOURCE_DIR}/stdlib"
    XR_API_DEFENSE_FILE="${PROJECT_SOURCE_DIR}/tests/unit/api/source_repl_hosted/program.xr")
set_target_properties(test_api_defense PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_api_defense PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_api_defense PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_api_defense COMMAND test_api_defense)
set_tests_properties(test_api_defense PROPERTIES TIMEOUT 120 LABELS "unit;api;source;ownership;repl;original")
foreach(source_entry IN ITEMS test_dostring_module_defaults test_dostring_program_image)
    add_executable(${source_entry} api/${source_entry}.c)
    target_link_libraries(${source_entry} PRIVATE xray_source_api)
    target_include_directories(${source_entry} PRIVATE ${XRAY_COMMON_INCLUDES} ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(${source_entry} PRIVATE XR_PUBLIC_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
    set_target_properties(${source_entry} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    add_test(NAME ${source_entry} COMMAND $<TARGET_FILE:${source_entry}>)
endforeach()
target_compile_definitions(test_dostring_program_image PRIVATE
    XR_PROGRAM_IMAGE_TEMP_ROOT="${CMAKE_CURRENT_BINARY_DIR}")
set_tests_properties(test_dostring_module_defaults PROPERTIES LABELS "api;modules;defaults")
set_tests_properties(test_dostring_program_image PROPERTIES LABELS "api;modules;generics")
add_test(
    NAME test_api_inventory_param_modes
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/api/test_api_inventory_param_modes.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(test_api_inventory_param_modes PROPERTIES
    LABELS "api;metadata;tooling"
)
add_test(
    NAME test_api_inventory_diff
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/api/test_api_inventory_diff.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(test_api_inventory_diff PROPERTIES
    LABELS "api;metadata;tooling"
)
add_test(
    NAME test_api_inventory_enum_metadata
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/api/test_api_inventory_enum_metadata.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(test_api_inventory_enum_metadata PROPERTIES
    LABELS "api;metadata;tooling;enum"
)
add_test(NAME test_api_inventory_native_declarations
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/api/test_api_inventory_native_declarations.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(test_api_inventory_native_declarations PROPERTIES LABELS "api;metadata;tooling;native-types")
add_test(NAME test_api_inventory_predicates
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/api/test_api_inventory_predicates.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(test_api_inventory_predicates PROPERTIES LABELS "api;metadata;tooling;xir")
add_test(
    NAME test_param_mode_tooling_display
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/api/test_param_mode_tooling_display.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(test_param_mode_tooling_display PROPERTIES
    LABELS "api;metadata;tooling;lsp;mcp"
)
add_test(
    NAME test_param_mode_diagnostics
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/api/test_param_mode_diagnostics.py
            --xray $<TARGET_FILE:xray>
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(test_param_mode_diagnostics PROPERTIES
    LABELS "api;syntax;diagnostics;tooling"
)
add_test(
    NAME test_param_contract_frontend
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/api/test_param_contract_frontend.py
            --xray $<TARGET_FILE:xray>
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(test_param_contract_frontend PROPERTIES
    LABELS "frontend;syntax;analyzer;task-206"
    TIMEOUT 60
)
add_test(
    NAME test_param_contract_aot
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/api/test_param_contract_aot.py
            --xray $<TARGET_FILE:xray>
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(test_param_contract_aot PROPERTIES
    LABELS "aot;frontend;task-206"
    TIMEOUT 60
)
add_test(
    NAME test_string_native_error_abi
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/api/test_string_native_error_abi.py
            --xray $<TARGET_FILE:xray>
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(test_string_native_error_abi PROPERTIES
    LABELS "vm;aot;frontend;task-198"
    TIMEOUT 120
)
add_test(
    NAME test_compress_native_error_abi
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/api/test_compress_native_error_abi.py
            --xray $<TARGET_FILE:xray>
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(test_compress_native_error_abi PROPERTIES
    LABELS "vm;aot;frontend;stdlib;compress;task-198"
    TIMEOUT 120
)
add_test(
    NAME test_crypto_native_error_abi
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/api/test_crypto_native_error_abi.py
            --xray $<TARGET_FILE:xray>
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(test_crypto_native_error_abi PROPERTIES
    LABELS "vm;aot;frontend;stdlib;crypto;task-198"
    TIMEOUT 120
)


# ============================================================================
# lsp/ — Language Server Protocol (conditional)
# ============================================================================

if(XR_BUILD_LSP)
    add_executable(test_lsp_json
        lsp/test_lsp_json.c
        ${CMAKE_SOURCE_DIR}/src/base/xjson.c
        ${CMAKE_SOURCE_DIR}/src/base/xutf8.c
    )
    target_include_directories(test_lsp_json PRIVATE
        ${CMAKE_SOURCE_DIR}/src/base
    )
    add_test(NAME test_lsp_json COMMAND test_lsp_json)

    add_executable(test_lsp_stdlib
        lsp/test_lsp_stdlib.c
        ${CMAKE_SOURCE_DIR}/src/app/lsp/xlsp_stdlib.c
    )
    target_include_directories(test_lsp_stdlib PRIVATE
        ${XRAY_COMMON_INCLUDES}
        ${CMAKE_SOURCE_DIR}/src/app/lsp
    )
    add_test(NAME test_lsp_stdlib COMMAND test_lsp_stdlib)
    add_test(NAME test_lsp_stdlib_generator COMMAND ${XRAY_PYTHON} -B -X utf8
        ${CMAKE_CURRENT_SOURCE_DIR}/lsp/test_gen_stdlib_types.py)
    set_tests_properties(test_lsp_stdlib_generator PROPERTIES LABELS "unit;lsp;metadata" TIMEOUT 30)

    file(GLOB LSP_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/app/lsp/*.c)
    list(FILTER LSP_SOURCES EXCLUDE REGEX "\\.inc\\.c$")
    list(REMOVE_ITEM LSP_SOURCES
        ${CMAKE_SOURCE_DIR}/src/app/lsp/xlsp_main.c
        ${CMAKE_SOURCE_DIR}/src/app/lsp/xlsp_source_query.c
        ${CMAKE_SOURCE_DIR}/src/app/lsp/xlsp_source_navigation.c
        ${CMAKE_SOURCE_DIR}/src/app/lsp/xlsp_source_workspace.c
        ${CMAKE_SOURCE_DIR}/src/app/lsp/xlsp_source_request.c
        ${CMAKE_SOURCE_DIR}/src/app/lsp/xlsp_source_code_lens.c
        ${CMAKE_SOURCE_DIR}/src/app/lsp/xlsp_source_syntax.c
        ${CMAKE_SOURCE_DIR}/src/app/lsp/xlsp_source_buffer.c
        ${CMAKE_SOURCE_DIR}/src/app/lsp/xlsp_source_open.c
        ${CMAKE_SOURCE_DIR}/src/app/lsp/xlsp_document_store.c)
    # Module-graph authority is shared with CLI source commands and is not part
    # of xray_core. Production xray already links these sources; full-source
    # LSP unit binaries must name them as well.
    list(APPEND LSP_SOURCES
        ${CMAKE_SOURCE_DIR}/src/app/cli/xcli_fs.c
        ${CMAKE_SOURCE_DIR}/src/app/cli/xcli_output.c)

    # The full-source LSP tests exercise the same Windows CRT-pipe adapters as
    # the production server; keep them enabled on every host.

    add_test(NAME test_lsp_protocol_transcript
        COMMAND ${XRAY_PYTHON}
            ${CMAKE_SOURCE_DIR}/tests/lsp/test_protocol_transcript.py
            --xray $<TARGET_FILE:xray>
    )
    set_tests_properties(test_lsp_protocol_transcript PROPERTIES TIMEOUT 20)

endif()

# ============================================================================
# dap/ — Debug Adapter Protocol (conditional)
# ============================================================================

if(XR_BUILD_DAP)
    add_subdirectory(dap/current)
    add_executable(test_dap_server
        dap/test_dap_server.c
        ${CMAKE_SOURCE_DIR}/src/base/xjson.c
        ${CMAKE_SOURCE_DIR}/src/base/xutf8.c
    )
    target_include_directories(test_dap_server PRIVATE
        ${CMAKE_SOURCE_DIR}/src/base
        ${CMAKE_SOURCE_DIR}/src/app/dap
    )
    add_test(NAME test_dap_server COMMAND test_dap_server)

    if(WIN32)
        set(_dap_fd_impl ${CMAKE_SOURCE_DIR}/src/os/win/fd_win.c)
    else()
        set(_dap_fd_impl ${CMAKE_SOURCE_DIR}/src/os/unix/fd_unix.c)
    endif()
    add_executable(test_dap_transport
        dap/test_dap_transport.c
        ${CMAKE_SOURCE_DIR}/src/app/dap/xdap_transport.c
        ${CMAKE_SOURCE_DIR}/src/base/xframing.c
        ${_dap_fd_impl}
    )
    target_include_directories(test_dap_transport PRIVATE
        ${CMAKE_SOURCE_DIR}/src/app/dap
        ${CMAKE_SOURCE_DIR}/src/base
    )
    add_test(NAME test_dap_transport COMMAND test_dap_transport)
endif()

# ============================================================================
# mcp/ — MCP protocol, tools, resources, prompts
# ============================================================================

if(XR_BUILD_MCP)
    include(mcp/knowledge.cmake)
    include(mcp/source_tools.cmake)

    add_executable(test_mcp_transport
        mcp/test_mcp_transport.c
        ${CMAKE_SOURCE_DIR}/src/app/mcp/xmcp_transport_stdio.c
    )
    target_include_directories(test_mcp_transport PRIVATE
        ${XRAY_COMMON_INCLUDES}
        ${CMAKE_SOURCE_DIR}/src/app/mcp
        ${CMAKE_SOURCE_DIR}/src/base
    )
    add_test(NAME test_mcp_transport COMMAND test_mcp_transport)

    add_executable(test_mcp_protocol mcp/test_mcp_protocol.c)
    target_link_libraries(test_mcp_protocol PRIVATE xray_mcp)
    target_include_directories(test_mcp_protocol PRIVATE ${XRAY_COMMON_INCLUDES})
    set(XR_MCP_PROTOCOL_TEST_GROUPS
        core
        tools
        format
        analyze
        run_basic
        run_deadline
        run_import_net
        run_import_http
        run_import_io
        run_import_os
        run_import_process
        run_import_cluster
        run_import_ws
        run_import_path
        knowledge_tools
        resources
        resource_templates
        prompts
        dispatch
    )
    foreach(group IN LISTS XR_MCP_PROTOCOL_TEST_GROUPS)
        add_custom_target(test_mcp_protocol_${group} DEPENDS test_mcp_protocol)
        add_test(
            NAME test_mcp_protocol_${group}
            COMMAND test_mcp_protocol --group ${group}
        )
        set_tests_properties(test_mcp_protocol_${group} PROPERTIES
            LABELS "unit;mcp-protocol;sharded"
        )
    endforeach()
    foreach(group IN LISTS XR_MCP_PROTOCOL_TEST_GROUPS)
        set_tests_properties(test_mcp_protocol_${group} PROPERTIES
            ENVIRONMENT "XRAY_STDLIB_PATH=${CMAKE_SOURCE_DIR}/stdlib")
    endforeach()
    add_test(NAME test_mcp_knowledge_generation
        COMMAND ${XRAY_PYTHON}
            ${CMAKE_SOURCE_DIR}/tests/mcp/test_knowledge_generation.py
            --root ${CMAKE_SOURCE_DIR}
            --xray $<TARGET_FILE:xray>
    )
    add_test(NAME test_mcp_protocol_transcript
        COMMAND ${XRAY_PYTHON}
            ${CMAKE_SOURCE_DIR}/tests/mcp/test_protocol_transcript.py
            --xray $<TARGET_FILE:xray>
    )
    set_tests_properties(test_mcp_knowledge_generation test_mcp_protocol_transcript
        PROPERTIES ENVIRONMENT "XRAY_STDLIB_PATH=${CMAKE_SOURCE_DIR}/stdlib")
endif()

# ============================================================================
# cli/ — CLI spec, parser, dispatch, help
# ============================================================================

add_test(NAME test_program_native_entropy_cli
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/cli/test_program_native_entropy.py
            --xray $<TARGET_FILE:xray>)
set_tests_properties(test_program_native_entropy_cli PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    LABELS "integration;cli;canonical-program;native;provider;crypto"
    TIMEOUT 600 COST 180)

add_test(NAME test_conversion_inventory
    COMMAND ${XRAY_PYTHON}
        ${CMAKE_SOURCE_DIR}/tests/unit/api/test_conversion_inventory.py
        $<TARGET_FILE:xray>
        ${CMAKE_SOURCE_DIR}/tests/diff/cases/semantics/int/contextual_numeric_conversion.xr
)
set_tests_properties(test_conversion_inventory PROPERTIES
    LABELS "api;cli;analyzer;numeric;task-242"
    PROCESSORS 1
    TIMEOUT 30
)

add_executable(test_cli_spec
    cli/test_cli_spec.c
    ${CMAKE_SOURCE_DIR}/src/app/cli/xcli_spec.c
    ${CMAKE_SOURCE_DIR}/src/app/cli/xcli_parser.c
    ${CMAKE_SOURCE_DIR}/src/app/cli/xcli_help.c
    ${CMAKE_SOURCE_DIR}/src/app/cli/xcli_diag.c
    ${CMAKE_SOURCE_DIR}/src/app/cli/xcli_output.c
)
target_include_directories(test_cli_spec PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_BINARY_DIR}/generated
    ${CMAKE_SOURCE_DIR}/src/app/cli
)
if(WIN32)
    target_sources(test_cli_spec PRIVATE
        ${CMAKE_SOURCE_DIR}/src/os/win/fd_win.c
        ${CMAKE_SOURCE_DIR}/src/os/win/time_win.c)
else()
    target_sources(test_cli_spec PRIVATE
        ${CMAKE_SOURCE_DIR}/src/os/unix/fd_unix.c
        ${CMAKE_SOURCE_DIR}/src/os/unix/time_unix.c)
endif()
set_target_properties(test_cli_spec PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(test_cli_spec PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_cli_spec PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME test_cli_spec COMMAND test_cli_spec)

include(cli/toolchain_process_owned.cmake)
include(cli/toolchain_algorithms.cmake)
if(WIN32)
    add_subdirectory(cli/dependency_facts)
    add_subdirectory(cli/process_image_observer)
    add_subdirectory(cli/image_collector_owner)
    add_subdirectory(cli/process_clone_observed)
    add_subdirectory(cli/file_lease_owner)
endif()

# ============================================================================
# test_harness/ — xray test runner self-tests
# Verify that @test discover, execution, pass/fail/skip, and exit codes
# all behave correctly.  These run the xray binary, not C unit tests.
# ============================================================================

set(_HARNESS_DIR ${CMAKE_SOURCE_DIR}/tests/test_harness)

add_test(NAME harness_single_pass
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/cli/test_harness_report_oracle.py
            --cli $<TARGET_FILE:xray>
            --fixture ${_HARNESS_DIR}/single_pass.xr
            --case single_pass
            --output ${CMAKE_CURRENT_BINARY_DIR}/harness-report-oracle/single_pass)
add_test(NAME harness_multi_pass
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/cli/test_harness_report_oracle.py
            --cli $<TARGET_FILE:xray>
            --fixture ${_HARNESS_DIR}/multi_pass.xr
            --case multi_pass
            --output ${CMAKE_CURRENT_BINARY_DIR}/harness-report-oracle/multi_pass)
add_test(NAME harness_with_skip
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/cli/test_harness_report_oracle.py
            --cli $<TARGET_FILE:xray>
            --fixture ${_HARNESS_DIR}/with_skip.xr
            --case with_skip
            --output ${CMAKE_CURRENT_BINARY_DIR}/harness-report-oracle/with_skip)
add_test(NAME harness_single_fail
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/cli/test_harness_report_oracle.py
            --cli $<TARGET_FILE:xray>
            --fixture ${_HARNESS_DIR}/single_fail.xr
            --case single_fail
            --output ${CMAKE_CURRENT_BINARY_DIR}/harness-report-oracle/single_fail)
add_test(NAME harness_no_tests
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/cli/test_harness_report_oracle.py
            --cli $<TARGET_FILE:xray>
            --fixture ${_HARNESS_DIR}/no_tests.xr
            --case no_tests
            --output ${CMAKE_CURRENT_BINARY_DIR}/harness-report-oracle/no_tests)
add_test(NAME harness_mixed_pass_fail
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/cli/test_harness_report_oracle.py
            --cli $<TARGET_FILE:xray>
            --fixture ${_HARNESS_DIR}/mixed_pass_fail.xr
            --case mixed_pass_fail
            --output ${CMAKE_CURRENT_BINARY_DIR}/harness-report-oracle/mixed_pass_fail)
set_tests_properties(harness_single_pass harness_multi_pass harness_with_skip
                     harness_single_fail harness_no_tests harness_mixed_pass_fail
    PROPERTIES WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

# ============================================================================
# Reverse-invariant checks for the Xi IR layer (static grep-based gates).
# Runs as part of ctest so PRs that introduce raw malloc, phase coordinates
# in source, or fallback mode flags are blocked locally before CI even runs.
# ============================================================================
add_test(
    NAME check_xi_invariants
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/scripts/check_xi_invariants.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

add_test(
    NAME check_rc_naming
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/scripts/check_rc_naming.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(check_rc_naming PROPERTIES
    LABELS "memory;rc;naming"
)

add_test(
    NAME check_xi_template_lowering
    COMMAND ${XRAY_PYTHON}
        ${CMAKE_SOURCE_DIR}/scripts/check_xi_template_lowering.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# These fixtures keep large runtime/authority records on the stack or run the
# complete compiler. Match the Windows driver budget without reducing ASan's.

# Debug sanitizer execution includes the complete numerical witness corpus and
# source module specialization. Schedule these measured long tests early.
if(ENABLE_ASAN)
    set_tests_properties(test_xir_execution PROPERTIES COST 120)
    # The exhaustive source-owner sweep takes about 280 seconds when run alone.
    # Preserve every injection site and allow for sanitizer contention.
    set_tests_properties(test_xir_source_allocations PROPERTIES COST 300 TIMEOUT 600)
    set_tests_properties(test_dostring_program_image PROPERTIES COST 230)
endif()

# Include every test, even when new declarations are appended later.
cmake_language(DEFER CALL xray_prepend_windows_asan_runtime_to_directory_tests)

# Compile the actual Source coordinators with an allocation-only test boundary.

# All production consumers still use the same fresh core. Only these two
# exact owners receive the test allocator; no production test-mode branch.

add_test(NAME test_callable52_wire_encoder
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/plan/derive_callable52_wire.py)

add_executable(test_source_compile_transaction api/test_source_compile_transaction.c)
target_link_libraries(test_source_compile_transaction PRIVATE xray_source_api)
target_include_directories(test_source_compile_transaction PRIVATE
    "${PROJECT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(test_source_compile_transaction PRIVATE
    XR_TRANSACTION_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
set_target_properties(test_source_compile_transaction PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_compile_transaction PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_source_compile_transaction PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_source_compile_transaction COMMAND test_source_compile_transaction)
set_tests_properties(test_source_compile_transaction PROPERTIES
    LABELS "unit;api;source;ownership;ast;original;transaction" TIMEOUT 120)

add_executable(test_source_repl_generation api/test_source_repl_generation.c)
target_link_libraries(test_source_repl_generation PRIVATE xray_source_api)
target_include_directories(test_source_repl_generation PRIVATE
    "${PROJECT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(test_source_repl_generation PRIVATE
    XR_GENERATION_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
set_target_properties(test_source_repl_generation PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_repl_generation PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_source_repl_generation PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_source_repl_generation COMMAND test_source_repl_generation)
set_tests_properties(test_source_repl_generation PROPERTIES
    LABELS "unit;api;source;ownership;repl;original;generation" TIMEOUT 120)

# Exercise the original publication obligation against actual Source owners.
add_executable(test_repl_publication_allocations api/test_repl_publication_allocations.c)
target_link_libraries(test_repl_publication_allocations PRIVATE xray_source_api)
target_include_directories(test_repl_publication_allocations PRIVATE
    "${PROJECT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(test_repl_publication_allocations PRIVATE
    XR_PUBLICATION_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
set_target_properties(test_repl_publication_allocations PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_repl_publication_allocations PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_repl_publication_allocations PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_repl_publication_allocations COMMAND test_repl_publication_allocations)
set_tests_properties(test_repl_publication_allocations PROPERTIES
    LABELS "unit;api;source;ownership;repl;original;compiler-faults" RUN_SERIAL TRUE TIMEOUT 120)

include(${CMAKE_CURRENT_SOURCE_DIR}/xir/constraint_queue.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/type_access_scratch.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/iterator_types.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/generator_driver.cmake)
include("${CMAKE_CURRENT_SOURCE_DIR}/xir/error_flow_scratch.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/xir/provenance_budget_release.cmake")
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/declaration_proof_scratch.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/type_match_scratch.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/signature_shape_scratch.cmake)

include(${CMAKE_CURRENT_SOURCE_DIR}/xir/tuple_owner.cmake)
include(xir/tuple_values.cmake)

include(xir/tuple_source_compile_owner.cmake)

# Fresh whole compiler/runtime fault matrices; scheduling does not extend deadlines.
if(ENABLE_ASAN OR ENABLE_UBSAN)
    set_tests_properties(test_xir_cleanup_source PROPERTIES COST 702)
else()
    set_tests_properties(test_xir_cleanup_source PROPERTIES COST 160)
endif()

include(xir/atomic_admission_owner.cmake)

include(xir/atomic_a2_source.cmake)

include(xir/atomic_a2_retry.cmake)

include(xir/atomic_runtime_core.cmake)

include(xir/atomic_legacy_admission_owner.cmake)

include(xir/atomic_source_admission_owner.cmake)

include(module/module_graph_logical_source.cmake)

include(xir/atomic_a2_runtime_fault.cmake)

include(xir/semantic65_vectors.cmake)

include(xir/array_remove.cmake)

include(xir/array_resize.cmake)

include(xir/source_shadow_function.cmake)

include("${CMAKE_CURRENT_SOURCE_DIR}/xir/array_entries.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/program/source_product_consumers.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/xir/go_source.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/go_closure.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/task_scope_source.cmake")
add_executable(test_xir_scope_syntax frontend/test_scope_syntax.c)
target_link_libraries(test_xir_scope_syntax PRIVATE xray_frontend_format)
if(MSVC)
    target_compile_options(test_xir_scope_syntax PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_scope_syntax PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME xir_scope_syntax COMMAND test_xir_scope_syntax)
set_tests_properties(xir_scope_syntax PROPERTIES TIMEOUT 60 LABELS "unit;parser;ownership")

include("${CMAKE_CURRENT_LIST_DIR}/xir/task_executor.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/root_callback.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/root_signal.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/pipe_runtime/pipe_runtime.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/process_runtime/process_runtime.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/dylib_runtime/dylib_runtime.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/xir/task_product.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/xir/task_cancel_prefix.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/xir/go_pending_normal.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/xir/go_source_native_resources.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/xir/vm19_fault_runtime.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/xir/executor_first_failure.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/task_fault_order.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/task_bool.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/task_bool_native.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/xir/task_unit.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/task_unit_native.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/xir/library_generics.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/xir/stdlib_vm.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/xir/source_product_vm.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/xir/stdlib_resources.cmake")

include("${PROJECT_SOURCE_DIR}/tests/cli/xir_public_cli_product.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/stdlib_preparation.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/stdlib_builtin_methods.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/task_outcome_copy_controls.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/xir/range_check.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/array_fill.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/array_concat.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/xir/task_late_cancel_cross.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/xir/task_array_error.cmake")
include(xir/task_array_admission.cmake)

include("${CMAKE_CURRENT_LIST_DIR}/xir/byte_array_value.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/compress_suite.cmake")

add_executable(test_xir_array_capacity_core xir/test_xir_array_capacity_core.c)
target_include_directories(test_xir_array_capacity_core PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(test_xir_array_capacity_core PRIVATE xray_xir_scalar)
include("${CMAKE_CURRENT_LIST_DIR}/xir/p09_reserve_cow.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/p09_compile_domain.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/p09_return_copy.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/p09_string_append_probe.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/p09_string_append_performance.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/p09_string_mutation.cmake")
add_executable(test_xir_array_capacity_checked xir/test_xir_array_capacity_checked.c)
target_link_libraries(test_xir_array_capacity_checked PRIVATE xray_xir)
foreach(capacity_core_target test_xir_array_capacity_core test_xir_array_capacity_checked)
    if(MSVC)
        target_compile_options(${capacity_core_target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${capacity_core_target} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${capacity_core_target} COMMAND ${capacity_core_target})
    set_tests_properties(${capacity_core_target} PROPERTIES TIMEOUT 120 LABELS "unit;xir;array;ownership;budget")
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/xir/array_capacity.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/array_capacity_budget.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/array_capacity_native_contract.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/xir/native_projection70.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/xir/source_layout22.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/xir/root_effects.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/xir/root_go_consumption.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/xir/root_cause_trace.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/xir/root_identity_vm.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/xir/root_identity_vm_resources.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/xir/root_execution_native.cmake")

include(xir/callable_root_bounds.cmake)

include(xir/callable_root_source.cmake)
include(xir/source_copy_refinement.cmake)
include(xir/invocation_zero_inputs.cmake)
include(xir/effect_term_scratch.cmake)
include(xir/refinement_context_lifetime.cmake)

include(xir/program_root_permissions.cmake)

include(xir/callable70_checked_vectors.cmake)

include(xir/root_identity_vm_compiler_resources.cmake)
include(xir/instance_generations.cmake)
include(xir/instance_bindings.cmake)

include(xir/owned_string_source_resources.cmake)

include("${CMAKE_CURRENT_LIST_DIR}/xir/child_callable_permissions.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/xir/error_cell_index.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/xir/error_edge_snapshot.cmake")
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/program_call_permissions.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/child_callable_native.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/child_callable_mixed.cmake)
include("${CMAKE_CURRENT_SOURCE_DIR}/xir/mixed_task_faults.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/xir/call_observation.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/xir/task_scalar_results.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/xir/task_constructed_results.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/xir/task_closed_error_baseline.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/xir/task_parameters.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/xir/task_structs.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/xir/task_classes.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/xir/task_recursive_classes.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/xir/task_empty_results.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/xir/task_constructed_sdk.cmake")
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/root_parameters.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/root_parameter_vm.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/root_parameter_native.cmake)

if(WIN32)
    add_executable(test_lsp_source_workspace xir/test_lsp_source_workspace.c)
    target_link_libraries(test_lsp_source_workspace PRIVATE xray_lsp_document_open bcrypt)
    target_compile_options(test_lsp_source_workspace PRIVATE /W4 /WX /utf-8)
    add_test(NAME test_lsp_source_workspace COMMAND ${XRAY_PYTHON} -X utf8
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/test_lsp_source_workspace.py
        $<TARGET_FILE:test_lsp_source_workspace> ${PROJECT_SOURCE_DIR}/stdlib)
    set_tests_properties(test_lsp_source_workspace PROPERTIES TIMEOUT 300 COST 40
        RUN_SERIAL TRUE LABELS "unit;lsp;xir;ownership;budget")
    add_subdirectory(xir/lsp_source_index)
    add_subdirectory(xir/lsp_source_index_store)
    add_subdirectory(xir/lsp_index_pool)
    add_subdirectory(xir/lsp_code_lens)
    add_subdirectory(xir/lsp_semantic_consumers)
    add_subdirectory(xir/lsp_selection)
    add_subdirectory(xir/lsp_format)
    add_subdirectory(xir/lsp_imports)
    add_subdirectory(xir/lsp_links)
    add_subdirectory(xir/lsp_rename)
endif()

add_executable(test_lsp_document_store xir/test_lsp_document_store.c)
target_link_libraries(test_lsp_document_store PRIVATE xray_lsp_document_open)
if(MSVC)
    target_compile_options(test_lsp_document_store PRIVATE /W4 /WX)
else()
    target_compile_options(test_lsp_document_store PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_lsp_document_store COMMAND test_lsp_document_store)
set_tests_properties(test_lsp_document_store PROPERTIES TIMEOUT 60 LABELS "unit;lsp;ownership;allocation")
if(WIN32)
    add_executable(test_lsp_source_open xir/test_lsp_source_open.c)
    target_link_libraries(test_lsp_source_open PRIVATE xray_lsp_document_open bcrypt)
    target_compile_options(test_lsp_source_open PRIVATE /W4 /WX /utf-8)
    add_test(NAME test_lsp_source_open COMMAND ${XRAY_PYTHON} -X utf8
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/test_lsp_source_open.py $<TARGET_FILE:test_lsp_source_open> ${PROJECT_SOURCE_DIR}/stdlib)
    set_tests_properties(test_lsp_source_open PROPERTIES TIMEOUT 300 COST 40 RUN_SERIAL TRUE LABELS "unit;lsp;ownership;metadata;allocation")
    add_test(NAME test_lsp_source_diagnostic_routing COMMAND ${XRAY_PYTHON} -X utf8
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/test_lsp_source_open.py $<TARGET_FILE:test_lsp_source_open> ${PROJECT_SOURCE_DIR}/stdlib --diagnostic-routing-faults)
    set_tests_properties(test_lsp_source_diagnostic_routing PROPERTIES TIMEOUT 300 COST 40 RUN_SERIAL TRUE LABELS "unit;lsp;ownership;metadata;allocation")
    add_test(NAME test_lsp_source_request COMMAND ${XRAY_PYTHON} -X utf8
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/test_lsp_source_open.py $<TARGET_FILE:test_lsp_source_open> ${PROJECT_SOURCE_DIR}/stdlib --source-request-faults)
    set_tests_properties(test_lsp_source_request PROPERTIES TIMEOUT 300 COST 80 RUN_SERIAL TRUE LABELS "unit;lsp;ownership;metadata;allocation")
    add_executable(test_lsp_async_owner xir/test_lsp_async_owner.c)
    target_include_directories(test_lsp_async_owner PRIVATE ${XRAY_COMMON_INCLUDES})
    target_compile_options(test_lsp_async_owner PRIVATE /W4 /WX /utf-8)
    add_test(NAME test_lsp_async_owner COMMAND test_lsp_async_owner)
    set_tests_properties(test_lsp_async_owner PROPERTIES TIMEOUT 30 LABELS "unit;lsp;ownership;allocation")
    add_executable(test_lsp_source_worker xir/test_lsp_source_worker.c
        ${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_async.c ${PROJECT_SOURCE_DIR}/src/os/win/thread_win.c)
    target_link_libraries(test_lsp_source_worker PRIVATE xray_lsp_document_open bcrypt)
    target_include_directories(test_lsp_source_worker PRIVATE ${XRAY_COMMON_INCLUDES})
    target_compile_options(test_lsp_source_worker PRIVATE /W4 /WX /utf-8)
    add_test(NAME test_lsp_source_worker COMMAND ${XRAY_PYTHON} -X utf8
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/test_lsp_source_open.py $<TARGET_FILE:test_lsp_source_worker> ${PROJECT_SOURCE_DIR}/stdlib --source-request)
    set_tests_properties(test_lsp_source_worker PROPERTIES TIMEOUT 30 LABELS "unit;lsp;ownership;metadata")
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/xir/source_dependencies.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/xir/ctfe_leaf.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/source_comptime.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/json_syntax.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/json_scalar_object.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/json_integer_object.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/json_float.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/range_value.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/library_integer_bits.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/exported_i64_const.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/exported_f64_const.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/source_decimal_parse.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/json_float_native.cmake")

include(${CMAKE_CURRENT_SOURCE_DIR}/xir/ordinary_live_limit.cmake)
add_test(NAME test_xir_ctfe_leaf_resources COMMAND test_xir_ctfe_leaf --resources)
set_tests_properties(test_xir_ctfe_leaf_resources PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE COST 120
    LABELS "unit;xir;compiler;ctfe;resources;ownership;budget")

include(xir/library_state_atomic_i64.cmake)

include(xir/library_state_task_i64.cmake)

include(xir/prelude_source.cmake)

add_executable(test_xir_cell_provenance xir/test_xir_cell_provenance.c)
target_include_directories(test_xir_cell_provenance PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(test_xir_cell_provenance PRIVATE xray_xir)
add_test(NAME test_xir_cell_provenance COMMAND test_xir_cell_provenance)
set_tests_properties(test_xir_cell_provenance PROPERTIES LABELS "xir;ownership" TIMEOUT 120)
add_test(NAME test_xir_cell_final_runtime COMMAND test_xir_cell_provenance --final-runtime)
set_tests_properties(test_xir_cell_final_runtime PROPERTIES LABELS "xir;ownership;finite-control;runtime;cell" TIMEOUT 60)

include("${CMAKE_CURRENT_LIST_DIR}/xir/root_conditional.cmake")
include(xir/effect_ordinary_environment.cmake)
include(xir/source_function_boundary.cmake)
include("${CMAKE_CURRENT_LIST_DIR}/xir/effect_final_retirement.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/effect_final_read.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/effect_final_proof.cmake")

include(xir/cell_owner_source.cmake)

include(xir/cell_loan_lifecycle.cmake)

include(${CMAKE_CURRENT_SOURCE_DIR}/xir/vm_bind_table.cmake)

include("${CMAKE_CURRENT_LIST_DIR}/xir/product_consumers/source_static_methods_current_vm.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/xir/product_consumers/source_typed_library_receiver_current_vm_candidate.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/product_consumers/source_static_methods_current_packet_vm_candidate.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/xir/product_consumers/source_typed_library_receiver_current_packet_vm_candidate.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/xir/product_consumers/source_typed_library_receiver_current_native_mixed_candidate.cmake")

include(${CMAKE_CURRENT_SOURCE_DIR}/xir/stdlib_catalog.cmake)

include("${CMAKE_CURRENT_LIST_DIR}/xir/owned_instruction_inventory.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/move_values.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/xir/cfg_storage.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/cfg_dominance_order.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/xir/instance_domain_fuel.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/specialize_key_controls.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/error_cfg_scheduling.cmake")
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/stdlib_families.cmake)

include(${CMAKE_CURRENT_SOURCE_DIR}/xir/program_correspondence.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/program_input_storage.cmake)
add_subdirectory(xir/p10_reference)
add_subdirectory(xir/p11_ctfe_reference)
add_subdirectory(xir/ctfe_cleanup)
add_subdirectory(hir/literal)
add_subdirectory(hir/conversion)
add_subdirectory(hir/binary)
add_subdirectory(hir/unary)
add_subdirectory(hir/boolean)
add_subdirectory(hir/cast)
add_subdirectory(hir/nullable)
add_subdirectory(hir/body)
add_subdirectory(xir/global_callable)
add_subdirectory(frontend/nosuspend_type)
add_subdirectory(xir/typed_await)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_string_place.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/program_owned_receive_probe.cmake)

include("${CMAKE_CURRENT_LIST_DIR}/xir/lsp_document_position/CMakeLists.txt")
include(xir/lsp_reexports/CMakeLists.txt)
include(xir/source_calls/CMakeLists.txt)
include(xir/lsp_calls/CMakeLists.txt)
include(xir/lsp_scope/CMakeLists.txt)
include(xir/source_types/CMakeLists.txt)
include(xir/lsp_types/CMakeLists.txt)
include(xir/lsp_alias_spans/CMakeLists.txt)

include(xir/source_cancel/CMakeLists.txt)
include(xir/lsp_source_task/CMakeLists.txt)
include(xir/lsp_json_clone/CMakeLists.txt)

include(xir/lsp_hierarchy_task/CMakeLists.txt)

include(xir/lsp_semantic_task/CMakeLists.txt)
include(xir/lsp_links_task/CMakeLists.txt)
include(xir/lsp_navigation_admission/CMakeLists.txt)

include(xir/lsp_call_syntax/CMakeLists.txt)

include(xir/lsp_signature_task/CMakeLists.txt)
include(xir/lsp_parameter_task/CMakeLists.txt)

include(xir/lsp_envelope/CMakeLists.txt)

include("${CMAKE_CURRENT_LIST_DIR}/xir/lsp_document_notification/CMakeLists.txt")

include("${CMAKE_CURRENT_LIST_DIR}/xir/lsp_diagnostic_queue/CMakeLists.txt")

include("${CMAKE_CURRENT_LIST_DIR}/xir/lsp_folding_task/CMakeLists.txt")

include("${CMAKE_CURRENT_LIST_DIR}/xir/lsp_json_decode/CMakeLists.txt")

include("${CMAKE_CURRENT_LIST_DIR}/xir/lsp_syntax_task/CMakeLists.txt")
include("${CMAKE_CURRENT_LIST_DIR}/xir/lsp_edit_task/CMakeLists.txt")
include("${CMAKE_CURRENT_LIST_DIR}/xir/lsp_workspace_symbol_task/CMakeLists.txt")
include("${CMAKE_CURRENT_LIST_DIR}/xir/lsp_rename_task/CMakeLists.txt")

include("${CMAKE_CURRENT_SOURCE_DIR}/xir/lsp_parse_diagnostics/CMakeLists.txt")

include("${CMAKE_CURRENT_SOURCE_DIR}/xir/lsp_diagnostic_publication/CMakeLists.txt")
include("${CMAKE_CURRENT_SOURCE_DIR}/xir/lsp_document_owner/CMakeLists.txt")

include("${CMAKE_CURRENT_LIST_DIR}/xir/lsp_completion_resolve/CMakeLists.txt")

include("${CMAKE_CURRENT_LIST_DIR}/xir/lsp_completion_resolve_errors/CMakeLists.txt")
include("${CMAKE_CURRENT_LIST_DIR}/xir/lsp_enum_payload/CMakeLists.txt")

add_subdirectory(xir/p11_generic_prefix)
add_subdirectory(xir/p11_source_receiving)
add_subdirectory(xir/p11_array_observe)
add_subdirectory(xir/p11_ref_closure_source)
add_subdirectory(xir/p11_struct_observe)
add_subdirectory(xir/p11_materialization)
add_subdirectory(xir/p11_recursive_data)
add_subdirectory(xir/p11_import)
add_subdirectory(xir/p11_ctfe_instance)
# Register independent legacy controls without replacing product-owned tests.
add_subdirectory(xir/legacy_consumers)
# Ordinary source classes retain the language receiver contract.
include(${CMAKE_CURRENT_SOURCE_DIR}/stdlib/class_receivers.cmake)

include(${CMAKE_CURRENT_LIST_DIR}/xir/error_contract_source.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/source_handler_facts.cmake)

add_executable(test_xir_retired_merge_packets xir/test_xir_retired_merge_packets.c)
target_link_libraries(test_xir_retired_merge_packets PRIVATE xray_xir)
if(MSVC)
    target_compile_options(test_xir_retired_merge_packets PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_retired_merge_packets PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_retired_merge_packets COMMAND test_xir_retired_merge_packets)
set_tests_properties(test_xir_retired_merge_packets PROPERTIES TIMEOUT 120 LABELS "xir;identity;ownership")
if(WIN32)
    add_test(NAME test_xir_runtime_sdk_known_bytes COMMAND test_xir_runtime_sdk --known-bytes)
    set_tests_properties(test_xir_runtime_sdk_known_bytes PROPERTIES TIMEOUT 120 LABELS "xir;sdk;identity;ownership")
endif()

add_test(NAME test_xir_combined_wire_smoke COMMAND test_xir_checked --wire-smoke)
set_tests_properties(test_xir_combined_wire_smoke PROPERTIES TIMEOUT 120 LABELS "unit;xir;checked;independent")

include(xir/source_history_class.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_graph_quiescence.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_keys.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_hash.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_hash_source.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_hash_contains.cmake)

include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_hash_members.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_map_index.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_hash_projection.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_set_algebra.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_map_value.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_map_membership.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_hash_foreach.cmake)

include(xir/p09_hash_sendable.cmake)

add_test(NAME test_xir_source_array_reorder_query COMMAND test_xir_source_arrays --query-only)
set_tests_properties(test_xir_source_array_reorder_query PROPERTIES TIMEOUT 120 LABELS "unit;xir;source;query;target-impact")

include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_map_key_path.cmake)

include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_source_borrow.cmake)

include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_read_loan.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_view_types.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_view_origins.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_view_affine.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_view_liveness.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_view_roots.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_view_root_resolver.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_view_loan_origins.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_view_access.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_view_owner_path.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_call_views.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_local_call_views.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_source_views.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_view_arguments.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_view_returns.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_view_moves.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_view_prepare.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_view_normalize.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_view_storage.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_view_values.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_view_owner_state.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_view_may_owned.cmake)
include(xir/union86.cmake)

add_executable(test_xir_shared_interfaces xir/test_xir_shared_interfaces.c)
target_link_libraries(test_xir_shared_interfaces PRIVATE xray_xir_scalar)
target_include_directories(test_xir_shared_interfaces PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(test_xir_shared_interfaces PRIVATE
    SHARED_PACKET_FILE="${PROJECT_SOURCE_DIR}/tests/unit/xir/shared_parent_packets.bin")
set_target_properties(test_xir_shared_interfaces PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_shared_interfaces PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_xir_shared_interfaces PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_shared_interfaces COMMAND test_xir_shared_interfaces)
add_test(NAME test_xir_shared_interfaces_vectors COMMAND ${XRAY_PYTHON}
    ${CMAKE_CURRENT_LIST_DIR}/xir/derive_shared_interfaces_vectors.py)
set_tests_properties(test_xir_shared_interfaces test_xir_shared_interfaces_vectors PROPERTIES
    TIMEOUT 120 LABELS "unit;xir;checked;independent;ownership")
include("${CMAKE_CURRENT_LIST_DIR}/xir/source_entry.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/never_type.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/local_errors.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/xir/local_cfg_errors.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/xir/error_union/CMakeLists.txt")

add_subdirectory(api/source_submission/correspondence)

add_subdirectory(xir/task_result)

include("${CMAKE_CURRENT_LIST_DIR}/xir/callable_array_transport.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/source_local_recursive_binding.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/xir/source_records.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/xir/source_method_surface.cmake")

include(${CMAKE_CURRENT_SOURCE_DIR}/xir/source_assert_throws.cmake)
include("${CMAKE_CURRENT_LIST_DIR}/xir/rest_values/rest_values.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/task_scope.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/task_aggregate.cmake")
foreach(engine vm native)
    add_test(NAME test_await_cancel_surface_${engine}
        COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/test_await_cancel_surface.py
            --root ${PROJECT_SOURCE_DIR} --binary $<TARGET_FILE:xray> --engine ${engine})
    set_tests_properties(test_await_cancel_surface_${engine} PROPERTIES
        LABELS "unit;xir;language;task;cli" TIMEOUT 180)
endforeach()
include("${CMAKE_CURRENT_LIST_DIR}/xir/call_select.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/xir/math_source.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/path_prelude.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/host_os/host_os.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/base/independent_tests.cmake")
