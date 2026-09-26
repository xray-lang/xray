# Unit tests for xray
# All unit tests are managed here (no glob in root CMakeLists.txt)

# ============================================================================
# Macro for adding unit tests that link to xray_core
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

macro(add_xray_unit_test name source)
    add_executable(${name} ${source} ${ARGN})
    target_compile_definitions(${name} PRIVATE XR_TEST_HAS_PROCESS_SHUTDOWN=1)
    target_include_directories(${name} PRIVATE
        ${XRAY_COMMON_INCLUDES}
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(${name} PRIVATE xray_core)
    add_test(NAME ${name} COMMAND $<TARGET_FILE:${name}>)
endmacro()

if(WIN32)
    add_executable(test_random_windows stdlib/test_random_windows.c
        ${CMAKE_SOURCE_DIR}/src/os/win/random_win.c)
    target_include_directories(test_random_windows PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_random_windows PRIVATE bcrypt)
    add_test(NAME test_random_windows COMMAND test_random_windows)
    set_tests_properties(test_random_windows PROPERTIES LABELS "unit;crypto;runtime;windows")
endif()

# Compiler-owner tests need source/module/compiler state but not the finalized
# stage-2 embedded stdlib. Keeping them on the bootstrap aggregate prevents an
# edit to the compiler or Program producer from rebuilding every stdlib
# bytecode and VM fastpath before the focused test can start.
macro(add_xray_bootstrap_executable name source)
    if(TARGET xray_core_objs AND TARGET xray_stdlib_bootstrap_core)
        add_executable(${name} ${source} ${ARGN})
        target_compile_definitions(${name} PRIVATE XR_TEST_HAS_PROCESS_SHUTDOWN=1)
        target_include_directories(${name} PRIVATE
            ${XRAY_COMMON_INCLUDES}
            ${CMAKE_CURRENT_SOURCE_DIR}
        )
        target_link_libraries(${name} PRIVATE
            xray_core_objs
            xray_stdlib_bootstrap_core)
    else()
        # The source-fixture build-graph self-test supplies a deliberately tiny
        # stand-alone project without the production object aggregates.
        add_executable(${name} ${source} ${ARGN})
    endif()
endmacro()

macro(add_xray_bootstrap_unit_test name source)
    add_xray_bootstrap_executable(${name} ${source} ${ARGN})
    add_test(NAME ${name} COMMAND $<TARGET_FILE:${name}>)
endmacro()

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

# ============================================================================
# value/ — Value system
# ============================================================================

add_executable(test_xir_stages xir/test_xir_stages.c)
target_link_libraries(test_xir_stages PRIVATE xray_xir)
if(MSVC)
    target_compile_options(test_xir_stages PRIVATE /W4 /WX)
else()
    target_compile_options(test_xir_stages PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_stages COMMAND test_xir_stages)
set_tests_properties(test_xir_stages PROPERTIES LABELS "unit;xir;metadata;ownership")
add_executable(test_xir_allocations xir/test_xir_allocations.c
    ${CMAKE_SOURCE_DIR}/src/aot/xi_cgen_verify_output.c)
target_include_directories(test_xir_allocations PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(MSVC)
    target_compile_options(test_xir_allocations PRIVATE /W4 /WX)
else()
    target_compile_options(test_xir_allocations PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_allocations COMMAND test_xir_allocations)
set_tests_properties(test_xir_allocations PROPERTIES LABELS "unit;xir;memory;ownership")

add_executable(test_xir_execution xir/test_xir_execution.c)
target_link_libraries(test_xir_execution PRIVATE xray_xir_vm)
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
set(XIR_SCALAR_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_scalar_fixture.c)
add_custom_command(OUTPUT ${XIR_SCALAR_GENERATED}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND $<TARGET_FILE:test_xir_emit> ${XIR_SCALAR_GENERATED}
    DEPENDS test_xir_emit
    VERBATIM)
add_executable(test_xir_native xir/test_xir_native.c ${XIR_SCALAR_GENERATED})
target_link_libraries(test_xir_native PRIVATE xray_xir_scalar)
foreach(xir_test IN ITEMS test_xir_execution test_xir_emit test_xir_native test_xir_calls
    test_xir_emit_calls test_xir_call_native)
    if(MSVC)
        target_compile_options(${xir_test} PRIVATE /W4 /WX)
    else()
        target_compile_options(${xir_test} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${xir_test} COMMAND ${xir_test})
    set_tests_properties(${xir_test} PROPERTIES LABELS "unit;xir;execution;ownership;abi")
endforeach()

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

add_library(xr_program_test_core STATIC
    ${CMAKE_SOURCE_DIR}/src/base/xsha256.c
    ${CMAKE_SOURCE_DIR}/src/core/xr_core_spec_gen.c
    ${CMAKE_SOURCE_DIR}/src/program/xr_core_ir.c
    ${CMAKE_SOURCE_DIR}/src/program/xr_program_diagnostic.c
    ${CMAKE_SOURCE_DIR}/src/program/xr_program_decode.c
    ${CMAKE_SOURCE_DIR}/src/program/xr_program_encode.c
    ${CMAKE_SOURCE_DIR}/src/program/xr_program_identity.c
    ${CMAKE_SOURCE_DIR}/src/program/xr_program_verify.c
    ${CMAKE_SOURCE_DIR}/src/program/xr_reference_evaluator.c
)
target_include_directories(xr_program_test_core PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(xr_program_test_core PUBLIC xray_program_vm_runtime)
add_dependencies(xr_program_test_core gen-core-spec gen-xr-program-schema)

add_executable(test_xr_program
    program/test_xr_program.c
    ${CMAKE_SOURCE_DIR}/src/program/xr_core_ir.c
    ${CMAKE_SOURCE_DIR}/src/program/xr_program_encode.c
    ${CMAKE_SOURCE_DIR}/src/program/xr_program_verify.c)
# Compile the real constructors and writer with allocator substitution confined to this
# executable. Other consumers link the ordinary production objects.
if(MSVC OR CMAKE_C_SIMULATE_ID STREQUAL "MSVC")
    target_compile_options(test_xr_program PRIVATE
        "/FI${CMAKE_CURRENT_SOURCE_DIR}/program/xr_program_allocation_probe.h")
else()
    target_compile_options(test_xr_program PRIVATE
        -include "${CMAKE_CURRENT_SOURCE_DIR}/program/xr_program_allocation_probe.h")
endif()
add_executable(test_xr_program_provider_requirements
    program/test_xr_program_provider_requirements.c)
target_include_directories(test_xr_program_provider_requirements PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(test_xr_program_provider_requirements PRIVATE xr_program_test_core)
add_test(NAME test_xr_program_provider_requirements
    COMMAND $<TARGET_FILE:test_xr_program_provider_requirements>)
set_tests_properties(test_xr_program_provider_requirements PROPERTIES
    LABELS "unit;canonical-program;program-format;semantic-verifier")
target_include_directories(test_xr_program PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(test_xr_program PRIVATE xr_program_test_core)
add_test(NAME test_xr_program COMMAND $<TARGET_FILE:test_xr_program>)
set_tests_properties(test_xr_program PROPERTIES
    LABELS "unit;canonical-program;program-format;task-296"
)

add_executable(test_xr_program_verify program/test_xr_program_verify.c)
add_executable(test_provider_logical_admission
    execution/test_provider_logical_admission.c plan/target_profile_test_fixture.c)
target_include_directories(test_provider_logical_admission PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(test_provider_logical_admission PRIVATE
    xr_program_test_core xray_program_vm_runtime xray_program_aot_compiler)
add_test(NAME test_provider_logical_admission COMMAND $<TARGET_FILE:test_provider_logical_admission>)
set_tests_properties(test_provider_logical_admission PROPERTIES
    LABELS "unit;provider;canonical-program;execution;aot")
target_include_directories(test_xr_program_verify PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(test_xr_program_verify PRIVATE
    xr_program_test_core xray_program_vm_runtime)
add_test(NAME test_xr_program_verify COMMAND $<TARGET_FILE:test_xr_program_verify>)
set_tests_properties(test_xr_program_verify PROPERTIES
    LABELS "unit;canonical-program;semantic-verifier;reference-evaluator;task-297"
)

set(XR_PROGRAM_SOURCE_FIXTURE_SCRIPT
    ${CMAKE_SOURCE_DIR}/scripts/program_source_fixtures.py)
set(XR_PROGRAM_SOURCE_FIXTURE_MANIFEST
    ${CMAKE_CURRENT_SOURCE_DIR}/program/xr_program_source_cases.json)
set(XR_PROGRAM_SOURCE_FIXTURE_DIR
    ${CMAKE_CURRENT_BINARY_DIR}/generated/program-source-fixtures)
set(XR_PROGRAM_SOURCE_FIXTURE_HEADER
    ${XR_PROGRAM_SOURCE_FIXTURE_DIR}/xr_program_source_cases.gen.h)
set(XR_PROGRAM_SOURCE_FIXTURE_REGISTRATION
    ${XR_PROGRAM_SOURCE_FIXTURE_DIR}/xr_program_source_fixtures.gen.cmake)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${XR_PROGRAM_SOURCE_FIXTURE_SCRIPT}
    ${XR_PROGRAM_SOURCE_FIXTURE_MANIFEST})
execute_process(
    COMMAND ${XRAY_PYTHON} ${XR_PROGRAM_SOURCE_FIXTURE_SCRIPT} project
        --header ${XR_PROGRAM_SOURCE_FIXTURE_HEADER}
        --cmake ${XR_PROGRAM_SOURCE_FIXTURE_REGISTRATION}
    COMMAND_ERROR_IS_FATAL ANY)

add_xray_bootstrap_executable(test_xr_program_source_build
    program/test_xr_program_source_build.c
    plan/target_profile_test_fixture.c)
# This test owns source -> XrProgram fixtures and independent VM/native oracles.
# Linking the final xray_core archive made its edit/build
# loop wait for the unrelated stage-2 stdlib bytecode and VM-fastpath
# generators. Use the same stage-1 aggregate as xray_stdlib_bcgen: all common
# objects plus the bootstrap embedded stdlib table. Full product and
# embedded-stdlib tests continue to link the final archive and retain their
# existing qualification boundary.
target_include_directories(test_xr_program_source_build PRIVATE
    ${XR_PROGRAM_SOURCE_FIXTURE_DIR})
target_compile_definitions(test_xr_program_source_build PRIVATE
    XR_OUTPUT_TRAP_NATIVE_HARNESS="${CMAKE_CURRENT_SOURCE_DIR}/aot/xr_program_output_trap_native_harness.c"
    XR_MODULE_REPORT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/module-report")
add_custom_command(TARGET test_xr_program_source_build PRE_LINK
    COMMAND ${XRAY_PYTHON} ${XR_PROGRAM_SOURCE_FIXTURE_SCRIPT} check
        --header ${XR_PROGRAM_SOURCE_FIXTURE_HEADER}
        --cmake ${XR_PROGRAM_SOURCE_FIXTURE_REGISTRATION}
    COMMENT "Checking source case census and native fixture projections"
    VERBATIM)
add_test(NAME xr_program_source_fixtures_self_test
    COMMAND ${XRAY_PYTHON} ${XR_PROGRAM_SOURCE_FIXTURE_SCRIPT} self-test)
set_tests_properties(xr_program_source_fixtures_self_test PROPERTIES
    LABELS "unit;canonical-program;source-owner;generator;self-test"
    COST 1)
add_test(NAME xr_program_source_fixtures_build_self_test
    COMMAND ${XRAY_PYTHON} ${XR_PROGRAM_SOURCE_FIXTURE_SCRIPT} self-test-build)
set_tests_properties(xr_program_source_fixtures_build_self_test PROPERTIES
    LABELS "unit;source-owner;generator;build-graph;self-test"
    TIMEOUT 60 PROCESSORS 1)
add_test(NAME test_xr_program_source_build
    COMMAND ${XRAY_PYTHON} ${XR_PROGRAM_SOURCE_FIXTURE_SCRIPT} run-sharded
        --producer $<TARGET_FILE:test_xr_program_source_build>
        --jobs 8)
set_tests_properties(test_xr_program_source_build PROPERTIES
    LABELS "unit;canonical-program;source-owner;module-graph;sharded;task-302"
    TIMEOUT 600 PROCESSORS 8 COST 180)

add_executable(test_xr_program_fuzz_entry ../fuzz/fuzz_xr_program_decode.c)
target_compile_definitions(test_xr_program_fuzz_entry PRIVATE FUZZ_STANDALONE)
target_include_directories(test_xr_program_fuzz_entry PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(test_xr_program_fuzz_entry PRIVATE xr_program_test_core)
add_test(NAME test_xr_program_fuzz_entry COMMAND $<TARGET_FILE:test_xr_program_fuzz_entry>)
set_tests_properties(test_xr_program_fuzz_entry PROPERTIES
    LABELS "unit;fuzz;canonical-program;program-format;hostile-input;task-296"
)

add_xray_unit_test(test_xvalue value/test_xvalue.c)


# ============================================================================
# util/ — Hash, dynamic array, hashmap, arena
# ============================================================================

add_xray_unit_test(test_xhash util/test_xhash.c)
add_xray_unit_test(test_dynarray util/test_dynarray.c)
add_xray_unit_test(test_hashmap util/test_hashmap.c)
add_xray_unit_test(test_arena util/test_arena.c)

# ============================================================================
# mem/ — RC memory model
# ============================================================================

add_xray_unit_test(test_region mem/test_region.c)
add_xray_unit_test(test_obj_header mem/test_obj_header.c)
add_xray_unit_test(test_fixed_heap_teardown mem/test_fixed_heap_teardown.c)
# A root-level deep copy lands on the fixed heap, so the copied array must
# carry an initialised accounting owner or teardown dereferences garbage.
add_xray_unit_test(test_deep_copy_array_owner_heap mem/test_deep_copy_array_owner_heap.c)
# The runtime half of `weak`, tested before any language
# surface exists for it (W1 read-promotes, W5 definite clearing point).
add_xray_unit_test(test_weak_handle mem/test_weak_handle.c)
# The whole-heap traversal is validated against
# heap->object_count before any cycle logic runs on top of it. The body is
# compiled out unless XR_ENABLE_CYCLE_DETECTOR is on, so the test is a no-op
# in a default build rather than a missing target.
add_xray_unit_test(test_cycle_detector_traversal mem/test_cycle_detector_traversal.c)
if(XR_ENABLE_CYCLE_DETECTOR)
    target_compile_definitions(test_cycle_detector_traversal PRIVATE XR_ENABLE_CYCLE_DETECTOR=1)
endif()
# The shared-domain scan (system-heap registry, dual-band trial
# deletion, channel/task edges) plus per-domain metering. Same no-op-by-default
# shape as the traversal test above.
add_xray_unit_test(test_cycle_detector_shared mem/test_cycle_detector_shared.c)
if(XR_ENABLE_CYCLE_DETECTOR)
    target_compile_definitions(test_cycle_detector_shared PRIVATE XR_ENABLE_CYCLE_DETECTOR=1)
endif()

add_xray_unit_test(test_execution_error_channel runtime/test_execution_error_channel.c)
set_tests_properties(test_execution_error_channel PROPERTIES
    LABELS "runtime;ownership;target-machine")

# These contract tests build only the low-level runtime modules. The focused
# validation loop stays independent of the full compiler archive. Ownership
# audit instrumentation is intentionally absent from release product archives.
add_executable(test_runtime_descriptor
    runtime/test_runtime_descriptor.c
    ${CMAKE_SOURCE_DIR}/src/runtime/abi/xr_runtime_descriptor.c
    ${CMAKE_SOURCE_DIR}/src/base/xsha256.c
)
target_include_directories(test_runtime_descriptor PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
add_test(NAME test_runtime_descriptor COMMAND $<TARGET_FILE:test_runtime_descriptor>)
set_tests_properties(test_runtime_descriptor PROPERTIES LABELS "runtime;abi;target-machine")

add_executable(test_runtime_abi_contract
    runtime/test_runtime_abi_contract.c
    ${CMAKE_SOURCE_DIR}/src/runtime/abi/xr_runtime_contract.c
    ${CMAKE_SOURCE_DIR}/src/runtime/abi/xr_provider_logical_contract.c
    ${CMAKE_SOURCE_DIR}/src/base/xsha256.c
)
target_include_directories(test_runtime_abi_contract PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
add_test(NAME test_runtime_abi_contract COMMAND $<TARGET_FILE:test_runtime_abi_contract>)
set_tests_properties(test_runtime_abi_contract PROPERTIES LABELS "runtime;abi;target-machine")

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
add_executable(test_stdlib_provider_binding
    execution/test_stdlib_provider_binding.c
    ${CMAKE_SOURCE_DIR}/src/execution/xr_stdlib_provider_binding.c
    ${CMAKE_SOURCE_DIR}/src/runtime/abi/xr_stdlib_provider_contract.c
    ${CMAKE_SOURCE_DIR}/src/runtime/abi/xr_provider_logical_contract.c
    ${CMAKE_SOURCE_DIR}/src/base/xsha256.c
    ${CMAKE_SOURCE_DIR}/src/plan/semantic/xr_semantic_ids.c
    ${XRAY_PROVIDER_TEST_HOST_SOURCES}
)
target_include_directories(test_stdlib_provider_binding PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
add_dependencies(test_stdlib_provider_binding gen-stdlib-metadata)
add_test(NAME test_stdlib_provider_binding COMMAND $<TARGET_FILE:test_stdlib_provider_binding>)
set_tests_properties(test_stdlib_provider_binding PROPERTIES LABELS "runtime;provider;unit")

add_executable(test_time_utc_offset runtime/test_time_utc_offset.c)
target_include_directories(test_time_utc_offset PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(NOT WIN32)
    target_compile_definitions(test_time_utc_offset PRIVATE _POSIX_C_SOURCE=200809L)
    target_compile_definitions(test_stdlib_provider_binding PRIVATE _POSIX_C_SOURCE=200809L)
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

add_executable(test_provider_logical_contract
    runtime/test_provider_logical_contract.c
    ${CMAKE_SOURCE_DIR}/src/runtime/abi/xr_provider_logical_contract.c
    ${CMAKE_SOURCE_DIR}/src/base/xsha256.c
)
target_include_directories(test_provider_logical_contract PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_provider_logical_contract COMMAND $<TARGET_FILE:test_provider_logical_contract>)
set_tests_properties(test_provider_logical_contract PROPERTIES
    LABELS "runtime;provider;canonical-program;unit"
)

add_executable(test_stdlib_provider_contract
    runtime/test_stdlib_provider_contract.c
    ${CMAKE_SOURCE_DIR}/src/runtime/abi/xr_stdlib_provider_contract.c
    ${CMAKE_SOURCE_DIR}/src/runtime/abi/xr_stdlib_provider_projection.c
    ${CMAKE_SOURCE_DIR}/src/runtime/abi/xr_provider_logical_contract.c
    ${CMAKE_SOURCE_DIR}/src/runtime/abi/xr_runtime_contract.c
    ${CMAKE_SOURCE_DIR}/src/runtime/abi/xr_runtime_target_authority.c
    ${CMAKE_SOURCE_DIR}/src/runtime/abi/xr_runtime_object_header.c
    ${CMAKE_SOURCE_DIR}/src/runtime/abi/xr_runtime_descriptor.c
    ${CMAKE_SOURCE_DIR}/src/runtime/abi/xr_runtime_string_object.c
    ${CMAKE_SOURCE_DIR}/src/plan/semantic/xr_semantic_ids.c
    ${CMAKE_SOURCE_DIR}/src/base/xtarget_data_layout.c
    ${CMAKE_SOURCE_DIR}/src/base/xsha256.c
)
target_include_directories(test_stdlib_provider_contract PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
add_dependencies(test_stdlib_provider_contract gen-stdlib-metadata)
add_test(NAME test_stdlib_provider_contract COMMAND $<TARGET_FILE:test_stdlib_provider_contract>)
set_tests_properties(test_stdlib_provider_contract PROPERTIES
    LABELS "runtime;provider;canonical-program;unit"
)

add_executable(test_runtime_object_header
    runtime/test_runtime_object_header.c
    ${CMAKE_SOURCE_DIR}/src/runtime/abi/xr_runtime_object_header.c
    ${CMAKE_SOURCE_DIR}/src/runtime/abi/xr_runtime_contract.c
    ${CMAKE_SOURCE_DIR}/src/runtime/abi/xr_provider_logical_contract.c
    ${CMAKE_SOURCE_DIR}/src/base/xsha256.c
)
target_include_directories(test_runtime_object_header PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
add_test(NAME test_runtime_object_header COMMAND $<TARGET_FILE:test_runtime_object_header>)
set_tests_properties(test_runtime_object_header PROPERTIES LABELS "runtime;abi;target-machine")

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

add_executable(test_strbuf_allocations runtime/test_strbuf_allocations.c)
target_include_directories(test_strbuf_allocations PRIVATE ${XRAY_COMMON_INCLUDES})
add_test(NAME test_strbuf_allocations COMMAND $<TARGET_FILE:test_strbuf_allocations>)
set_tests_properties(test_strbuf_allocations PROPERTIES LABELS "runtime;string;allocation-failure")

add_executable(test_byte_compare_core runtime/test_byte_compare_core.c)
target_include_directories(test_byte_compare_core PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(MSVC OR (CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    target_compile_options(test_byte_compare_core PRIVATE /W4 /WX)
else()
    target_compile_options(test_byte_compare_core PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_byte_compare_core COMMAND $<TARGET_FILE:test_byte_compare_core>)
set_tests_properties(test_byte_compare_core PROPERTIES LABELS "unit;crypto;runtime;kernel")

add_executable(test_string_builder_storage runtime/test_string_builder_storage.c)
target_include_directories(test_string_builder_storage PRIVATE ${XRAY_COMMON_INCLUDES})
add_test(NAME test_string_builder_storage COMMAND $<TARGET_FILE:test_string_builder_storage>)
set_tests_properties(test_string_builder_storage PROPERTIES
    LABELS "runtime;string;canonical;ownership;allocation-failure")

add_executable(test_array_append_storage runtime/test_array_append_storage.c)
target_include_directories(test_array_append_storage PRIVATE ${XRAY_COMMON_INCLUDES})
add_test(NAME test_array_append_storage COMMAND $<TARGET_FILE:test_array_append_storage>)
set_tests_properties(test_array_append_storage PROPERTIES
    LABELS "runtime;array;canonical;ownership;allocation-failure")

add_executable(test_array_allocation_plan runtime/test_array_allocation_plan.c)
target_include_directories(test_array_allocation_plan PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_array_allocation_plan COMMAND $<TARGET_FILE:test_array_allocation_plan>)
set_tests_properties(test_array_allocation_plan PROPERTIES LABELS "runtime;array;canonical;allocation-failure")

add_executable(test_runtime_string_object
    runtime/test_runtime_string_object.c
    ${CMAKE_SOURCE_DIR}/src/runtime/abi/xr_runtime_string_object.c
    ${CMAKE_SOURCE_DIR}/src/runtime/abi/xr_runtime_object_header.c
    ${CMAKE_SOURCE_DIR}/src/runtime/abi/xr_runtime_descriptor.c
    ${CMAKE_SOURCE_DIR}/src/runtime/abi/xr_runtime_contract.c
    ${CMAKE_SOURCE_DIR}/src/runtime/abi/xr_provider_logical_contract.c
    ${CMAKE_SOURCE_DIR}/src/plan/semantic/xr_semantic_ids.c
    ${CMAKE_SOURCE_DIR}/src/base/xsha256.c
)
target_include_directories(test_runtime_string_object PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
add_test(NAME test_runtime_string_object COMMAND $<TARGET_FILE:test_runtime_string_object>)
set_tests_properties(test_runtime_string_object PROPERTIES
    LABELS "runtime;abi;object;string;target-machine")

add_test(
    NAME check_runtime_string_object_boundary
    COMMAND ${XRAY_PYTHON}
        ${CMAKE_SOURCE_DIR}/scripts/check_runtime_string_object_boundary.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(check_runtime_string_object_boundary PROPERTIES
    LABELS "runtime;abi;object;string;static-analysis;target-machine")

add_test(
    NAME check_runtime_object_header_boundary
    COMMAND ${XRAY_PYTHON}
        ${CMAKE_SOURCE_DIR}/scripts/check_runtime_object_header_boundary.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(check_runtime_object_header_boundary PROPERTIES
    LABELS "runtime;abi;static-analysis;target-machine")

add_executable(test_ownership_audit
    runtime/test_ownership_audit.c
    ${CMAKE_SOURCE_DIR}/src/runtime/abi/xr_runtime_descriptor.c
    ${CMAKE_SOURCE_DIR}/src/runtime/ownership/xr_ownership_audit.c
    ${CMAKE_SOURCE_DIR}/src/base/xsha256.c
)
target_include_directories(test_ownership_audit PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_definitions(test_ownership_audit PRIVATE
    XR_OWNERSHIP_AUDIT_TESTING=1
)
if(NOT WIN32)
    target_link_libraries(test_ownership_audit PRIVATE pthread)
endif()
add_test(NAME test_ownership_audit COMMAND $<TARGET_FILE:test_ownership_audit>)
set_tests_properties(test_ownership_audit PROPERTIES
    LABELS "runtime;ownership;concurrency;target-machine")

add_test(
    NAME check_ownership_audit_record_no_alloc
    COMMAND ${XRAY_PYTHON}
        ${CMAKE_SOURCE_DIR}/scripts/check_ownership_audit_record_no_alloc.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(check_ownership_audit_record_no_alloc PROPERTIES
    LABELS "runtime;ownership;static-analysis;target-machine"
)

add_test(
    NAME check_ownership_audit_release_boundary
    COMMAND ${XRAY_PYTHON}
        ${CMAKE_SOURCE_DIR}/scripts/check_ownership_audit_release_boundary.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(check_ownership_audit_release_boundary PROPERTIES
    LABELS "runtime;ownership;static-analysis;target-machine"
)

# ============================================================================
# object/ — Runtime objects (string, array, utf8)
# ============================================================================

add_xray_unit_test(test_utf8_diagnostic base/test_utf8_diagnostic.c)
add_xray_unit_test(test_utf8 object/test_utf8.c)

add_xray_unit_test(test_xarray object/test_xarray.c)
add_xray_unit_test(test_dynamic_class object/test_dynamic_class.c)
add_xray_unit_test(test_xstring object/test_xstring.c)
add_xray_unit_test(test_xmap object/test_xmap.c)
add_xray_unit_test(test_xset object/test_xset.c)
add_xray_unit_test(test_xtuple object/test_xtuple.c)
add_xray_unit_test(test_rc_container_release object/test_rc_container_release.c)

# ============================================================================
# frontend/ — Lexer, parser, diagnostics
# ============================================================================

add_xray_unit_test(test_lexer frontend/test_lexer.c)
add_xray_unit_test(test_lexer_keywords frontend/test_lexer_keywords.c)
add_xray_unit_test(test_lexer_positions frontend/test_lexer_positions.c)
add_xray_unit_test(test_lexer_trivia frontend/test_lexer_trivia.c)
add_xray_unit_test(test_parser frontend/test_parser.c)
add_xray_unit_test(test_parser_type_scope frontend/test_parser_type_scope.c)
add_xray_bootstrap_unit_test(test_parser_recoverable frontend/test_parser_recoverable.c)
add_xray_unit_test(test_parser_asi frontend/test_parser_asi.c)
add_xray_unit_test(test_diag_fmt frontend/test_diag_fmt.c)
add_xray_unit_test(test_xfmt_literal frontend/test_xfmt_literal.c)
add_xray_bootstrap_unit_test(test_formatter_comments frontend/test_formatter_comments.c)
add_xray_unit_test(test_formatter_strings frontend/test_formatter_strings.c)
add_xray_unit_test(test_fmt_roundtrip frontend/test_fmt_roundtrip.c)
add_xray_unit_test(test_analyzer_remove_file frontend/test_analyzer_remove_file.c)
add_xray_unit_test(test_analyzer_incremental frontend/test_analyzer_incremental.c)
add_xray_unit_test(test_xa_typed_program frontend/test_xa_typed_program.c)
add_xray_unit_test(test_xa_program_semantic_closure frontend/test_xa_program_semantic_closure.c)
add_xray_unit_test(test_scalar_call_decision plan/test_scalar_call_decision.c)
target_sources(test_scalar_call_decision PRIVATE plan/target_profile_test_fixture.c)
# Phase 3.5: X-01 AstNode -> XrType side table acceptance tests.
add_xray_unit_test(test_ast_side_table frontend/test_ast_side_table.c)
add_xray_unit_test(test_symbol_binding frontend/test_symbol_binding.c)
add_xray_unit_test(test_scope_tree frontend/test_scope_tree.c)
add_xray_unit_test(test_xa_selection frontend/test_xa_selection.c)
add_xray_unit_test(test_ast_visit_coverage frontend/test_ast_visit_coverage.c)
add_xray_unit_test(test_selection_facts frontend/test_selection_facts.c)

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

add_xray_bootstrap_unit_test(test_analyzer analyzer/test_analyzer.c)
add_test(NAME test_analyzer_strict_conditions COMMAND $<TARGET_FILE:test_analyzer> strict-conditions)
set_tests_properties(test_analyzer_strict_conditions PROPERTIES LABELS "analyzer;conditions;language")
add_xray_unit_test(test_effect_db analyzer/test_effect_db.c)
add_xray_unit_test(test_xrd_effect_metadata analyzer/test_xrd_effect_metadata.c)
add_xray_bootstrap_unit_test(test_mono analyzer/test_mono.c)
add_xray_unit_test(test_interface_conformance analyzer/test_interface_conformance.c)
add_xray_bootstrap_unit_test(test_xglobal_summary analysis/test_xglobal_summary.c)
add_xray_bootstrap_unit_test(test_xglobal_cache_payload analysis/test_xglobal_cache_payload.c)

# ============================================================================
# vm/ — Instruction encoding, opcode
# ============================================================================

add_xray_unit_test(test_instruction vm/test_instruction.c)
add_xray_unit_test(test_opcode vm/test_opcode.c)
add_xray_unit_test(test_bytecode_io vm/test_bytecode_io.c)
add_xray_unit_test(test_ffi_callback vm/test_ffi_callback.c)
add_xray_unit_test(test_ffi_scalar vm/test_ffi_scalar.c)
add_xray_unit_test(test_vm_api vm/test_vm_api.c)
add_xray_unit_test(test_run_closure_blocking vm/test_run_closure_blocking.c)
add_xray_unit_test(test_ic vm/test_ic.c)
# xray_core already PUBLIC-links pthread (coroutines need it), so the
# concurrent IC test inherits the dependency through xray_core.
add_xray_unit_test(test_ic_concurrent vm/test_ic_concurrent.c)
add_xray_unit_test(test_vm_exception vm/test_vm_exception.c)
add_xray_unit_test(test_vm_parallel_isolate vm/test_vm_parallel_isolate.c)
add_xray_unit_test(test_vm_grapheme_iterator vm/test_vm_grapheme_iterator.c)
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

add_xray_unit_test(test_symbol symbol/test_symbol.c)

# ============================================================================
# ir/ — New typed SSA IR (Xi)
# ============================================================================

add_xray_unit_test(test_xi ir/test_xi.c)
add_xray_unit_test(test_xi_lower ir/test_xi_lower.c)
add_xray_unit_test(test_quoted_literal_scaling ir/test_quoted_literal_scaling.c)
add_xray_unit_test(test_xi_lowering_gen ir/test_xi_lowering_gen.c)
add_xray_unit_test(test_xi_opt ir/test_xi_opt.c)
add_xray_unit_test(test_xi_analysis ir/test_xi_analysis.c)
add_xray_unit_test(test_xi_cfg_edit ir/test_xi_cfg_edit.c)
add_xray_unit_test(test_xi_emit ir/test_xi_emit.c)
add_xray_bootstrap_unit_test(test_xi_pipeline ir/test_xi_pipeline.c)
add_test(NAME test_xi_pipeline_canonical
    COMMAND $<TARGET_FILE:test_xi_pipeline> --canonical)
set_tests_properties(test_xi_pipeline_canonical PROPERTIES
    LABELS "unit;ir;canonical-program;source-owner;task-293;task-302")

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

set(XR_PROGRAM_SOURCE_AOT_FIXTURE_DIR
    ${CMAKE_CURRENT_BINARY_DIR}/generated/task-293-source-aot)
set(XR_PROGRAM_SOURCE_AOT_GENERATED_C
    ${XR_PROGRAM_SOURCE_AOT_FIXTURE_DIR}/source_aot_native.c)
set(XR_PROGRAM_POINTER_AOT_GENERATED_C
    ${XR_PROGRAM_SOURCE_AOT_FIXTURE_DIR}/pointer_aot_native.c)
set(XR_PROGRAM_YIELD_AOT_GENERATED_C
    ${XR_PROGRAM_SOURCE_AOT_FIXTURE_DIR}/yield_aot_native.c)
set(XR_PROGRAM_COROUTINE_CALL_AOT_GENERATED_C
    ${XR_PROGRAM_SOURCE_AOT_FIXTURE_DIR}/coroutine_call_aot_native.c)
math(EXPR XR_PROGRAM_NATIVE_POINTER_BITS "${CMAKE_SIZEOF_VOID_P} * 8")
add_custom_command(
    OUTPUT ${XR_PROGRAM_SOURCE_AOT_GENERATED_C} ${XR_PROGRAM_POINTER_AOT_GENERATED_C}
        ${XR_PROGRAM_YIELD_AOT_GENERATED_C} ${XR_PROGRAM_COROUTINE_CALL_AOT_GENERATED_C}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${XR_PROGRAM_SOURCE_AOT_FIXTURE_DIR}
    COMMAND $<TARGET_FILE:test_xi_pipeline>
        --source-aot-c ${XR_PROGRAM_SOURCE_AOT_GENERATED_C}
        --pointer-aot-c ${XR_PROGRAM_POINTER_AOT_GENERATED_C}
        --yield-aot-c ${XR_PROGRAM_YIELD_AOT_GENERATED_C}
        --coroutine-call-aot-c ${XR_PROGRAM_COROUTINE_CALL_AOT_GENERATED_C}
    DEPENDS test_xi_pipeline
    COMMENT "Generating source-backed existential canonical-program C fixture"
    VERBATIM)
set_source_files_properties(${XR_PROGRAM_SOURCE_AOT_GENERATED_C}
    PROPERTIES GENERATED TRUE)
add_executable(test_xr_program_source_aot_native
    ${XR_PROGRAM_SOURCE_AOT_GENERATED_C})
xr_enable_pure_aot_symbol_map(test_xr_program_source_aot_native)
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC"
        OR (CMAKE_C_COMPILER_ID STREQUAL "Clang"
            AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    target_compile_options(test_xr_program_source_aot_native PRIVATE /W4 /WX)
else()
    target_compile_options(test_xr_program_source_aot_native PRIVATE
        -std=c11 -pedantic-errors -Wall -Wextra -Werror)
endif()
add_test(NAME test_xr_program_source_aot_native
    COMMAND ${XRAY_PYTHON}
        ${CMAKE_SOURCE_DIR}/scripts/check_xr_program_aot_native.py
        --executable $<TARGET_FILE:test_xr_program_source_aot_native>
        --expected-exit 478)
set_tests_properties(test_xr_program_source_aot_native PROPERTIES
    LABELS "unit;aot;canonical-program;source;existential;witness;generated-c;native;task-293")
set_source_files_properties(${XR_PROGRAM_POINTER_AOT_GENERATED_C}
    PROPERTIES GENERATED TRUE)
add_executable(test_xr_program_pointer_aot_native
    ${XR_PROGRAM_POINTER_AOT_GENERATED_C})
xr_enable_pure_aot_symbol_map(test_xr_program_pointer_aot_native)
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC"
        OR (CMAKE_C_COMPILER_ID STREQUAL "Clang"
            AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    target_compile_options(test_xr_program_pointer_aot_native PRIVATE /W4 /WX)
else()
    target_compile_options(test_xr_program_pointer_aot_native PRIVATE
        -std=c11 -pedantic-errors -Wall -Wextra -Werror)
endif()
add_test(NAME test_xr_program_pointer_aot_native
    COMMAND ${XRAY_PYTHON}
        ${CMAKE_SOURCE_DIR}/scripts/check_xr_program_aot_native.py
        --executable $<TARGET_FILE:test_xr_program_pointer_aot_native>
        --expected-exit ${XR_PROGRAM_NATIVE_POINTER_BITS})
set_tests_properties(test_xr_program_pointer_aot_native PROPERTIES
    LABELS "unit;aot;canonical-program;source;target-profile;pointer-width;generated-c;native;task-293")
set_source_files_properties(${XR_PROGRAM_YIELD_AOT_GENERATED_C}
    PROPERTIES GENERATED TRUE)
add_executable(test_xr_program_yield_aot_native
    ${XR_PROGRAM_YIELD_AOT_GENERATED_C})
xr_enable_pure_aot_symbol_map(test_xr_program_yield_aot_native)
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC"
        OR (CMAKE_C_COMPILER_ID STREQUAL "Clang"
            AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    target_compile_options(test_xr_program_yield_aot_native PRIVATE /W4 /WX)
else()
    target_compile_options(test_xr_program_yield_aot_native PRIVATE
        -std=c11 -pedantic-errors -Wall -Wextra -Werror)
endif()
add_test(NAME test_xr_program_yield_aot_native
    COMMAND ${XRAY_PYTHON}
        ${CMAKE_SOURCE_DIR}/scripts/check_xr_program_aot_native.py
        --executable $<TARGET_FILE:test_xr_program_yield_aot_native>
        --expected-exit 0)
set_tests_properties(test_xr_program_yield_aot_native PROPERTIES
    LABELS "unit;aot;canonical-program;source;coroutine;yield;generated-c;native;task-293")
set_source_files_properties(${XR_PROGRAM_COROUTINE_CALL_AOT_GENERATED_C}
    PROPERTIES GENERATED TRUE)
add_executable(test_xr_program_coroutine_call_aot_native
    ${XR_PROGRAM_COROUTINE_CALL_AOT_GENERATED_C})
xr_enable_pure_aot_symbol_map(test_xr_program_coroutine_call_aot_native)
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC"
        OR (CMAKE_C_COMPILER_ID STREQUAL "Clang"
            AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    target_compile_options(test_xr_program_coroutine_call_aot_native PRIVATE /W4 /WX)
else()
    target_compile_options(test_xr_program_coroutine_call_aot_native PRIVATE
        -std=c11 -pedantic-errors -Wall -Wextra -Werror)
endif()
add_test(NAME test_xr_program_coroutine_call_aot_native
    COMMAND ${XRAY_PYTHON}
        ${CMAKE_SOURCE_DIR}/scripts/check_xr_program_aot_native.py
        --executable $<TARGET_FILE:test_xr_program_coroutine_call_aot_native>
        --expected-exit 7)
set_tests_properties(test_xr_program_coroutine_call_aot_native PROPERTIES
    LABELS "unit;aot;canonical-program;source;coroutine;call;generated-c;native;task-293")

set(XR_PROGRAM_SOURCE_module_report_INPUTS
    ${CMAKE_SOURCE_DIR}/tests/fixtures/module-report/main.xr
    ${CMAKE_SOURCE_DIR}/tests/fixtures/module-report/library.xr)
set(XR_PROGRAM_SOURCE_module_report_extended_INPUTS
    ${CMAKE_SOURCE_DIR}/tests/fixtures/module-report/extended.xr
    ${CMAKE_SOURCE_DIR}/tests/fixtures/module-report/facade.xr
    ${CMAKE_SOURCE_DIR}/tests/fixtures/module-report/library.xr)

# Each manifest fixture has one producer and one independent native qualification.
# An empty expected_stdout_hex only checks the exit byte; otherwise the native
# process must write exactly those bytes to stdout.
function(add_xr_program_source_native_fixture fixture_id native_target expected_exit fixture_labels
         expected_stdout_hex expected_stderr_hex)
    set(generated_c "${XR_PROGRAM_SOURCE_FIXTURE_DIR}/${fixture_id}_native.c")
    add_custom_command(
        OUTPUT "${generated_c}"
        COMMAND ${XRAY_PYTHON} ${XR_PROGRAM_SOURCE_FIXTURE_SCRIPT} generate
            --fixture "${fixture_id}"
            --producer $<TARGET_FILE:test_xr_program_source_build>
            --output "${generated_c}"
        DEPENDS test_xr_program_source_build
            ${XR_PROGRAM_SOURCE_FIXTURE_SCRIPT}
            ${XR_PROGRAM_SOURCE_FIXTURE_MANIFEST}
            ${XR_PROGRAM_SOURCE_${fixture_id}_INPUTS}
        COMMENT "Generating canonical-program native fixture ${fixture_id}"
        VERBATIM)
    set_source_files_properties("${generated_c}" PROPERTIES GENERATED TRUE)
    add_executable(${native_target} "${generated_c}")
    target_link_libraries(${native_target} PRIVATE xray_native_provider_runtime)
    xr_enable_pure_aot_symbol_map(${native_target})
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC"
            OR (CMAKE_C_COMPILER_ID STREQUAL "Clang"
                AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
        target_compile_options(${native_target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${native_target} PRIVATE
            -std=c11 -pedantic-errors -Wall -Wextra -Werror)
    endif()
    set(stdout_arguments)
    if(NOT expected_stdout_hex STREQUAL "")
        set(stdout_arguments --expected-stdout-hex ${expected_stdout_hex})
    endif()
    set(stderr_arguments)
    if(NOT expected_stderr_hex STREQUAL "")
        set(stderr_arguments --expected-stderr-hex ${expected_stderr_hex})
    endif()
    add_test(NAME ${native_target}
        COMMAND ${XRAY_PYTHON}
            ${CMAKE_SOURCE_DIR}/scripts/check_xr_program_aot_native.py
            --executable $<TARGET_FILE:${native_target}>
            --expected-exit ${expected_exit} ${stdout_arguments} ${stderr_arguments})
    set_tests_properties(${native_target} PROPERTIES
        LABELS "unit;aot;canonical-program;source;${fixture_labels};generated-c;native;task-293")
endfunction()

function(add_xr_program_source_pending_fixture fixture_id source_case test_name fixture_labels)
    add_test(NAME ${test_name}
        COMMAND $<TARGET_FILE:test_xr_program_source_build> --run-case ${source_case})
    set_tests_properties(${test_name} PROPERTIES
        LABELS "unit;canonical-program;source;${fixture_labels};backend-pending;task-293"
        TIMEOUT 180
        PROCESSORS 1)
endfunction()
set(XR_PROGRAM_SOURCE_module_error_output_INPUTS ${CMAKE_CURRENT_SOURCE_DIR}/aot/xr_program_output_trap_native_harness.c)
set(XR_PROGRAM_SOURCE_module_panic_output_INPUTS ${CMAKE_CURRENT_SOURCE_DIR}/aot/xr_program_output_trap_native_harness.c)
include(${XR_PROGRAM_SOURCE_FIXTURE_REGISTRATION})

foreach(fixture IN ITEMS value_struct_string module_initializer_panic_message)
    set(target test_xr_program_${fixture}_allocations)
    if(fixture STREQUAL "value_struct_string")
        set(expected_exit 42)
    else()
        set(expected_exit 1)
    endif()
    add_executable(${target} aot/xr_program_source_allocation_harness.c)
    add_dependencies(${target} test_xr_program_${fixture}_aot_native)
    target_include_directories(${target} PRIVATE ${XR_PROGRAM_SOURCE_FIXTURE_DIR})
    target_compile_definitions(${target} PRIVATE
        XR_SOURCE_NATIVE_FILE="${fixture}_native.c" XR_SOURCE_EXPECTED_EXIT=${expected_exit})
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -std=c11 -pedantic-errors -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES
        LABELS "unit;canonical-program;source;string;ownership;allocation-failure;native")
endforeach()

add_xray_unit_test(test_xr_program_imported_callable
    ir/test_xr_program_imported_callable.c)
set_tests_properties(test_xr_program_imported_callable PROPERTIES
    LABELS "unit;canonical-program;source;callable;multi-module;task-301")

set(XR_PROGRAM_IMPORTED_CALLABLE_FIXTURE_DIR
    ${CMAKE_CURRENT_BINARY_DIR}/generated/task-301-imported-callable)
set(XR_PROGRAM_IMPORTED_CALLABLE_GENERATED_C
    ${XR_PROGRAM_IMPORTED_CALLABLE_FIXTURE_DIR}/imported_callable_native.c)
add_custom_command(
    OUTPUT ${XR_PROGRAM_IMPORTED_CALLABLE_GENERATED_C}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${XR_PROGRAM_IMPORTED_CALLABLE_FIXTURE_DIR}
    COMMAND $<TARGET_FILE:test_xr_program_imported_callable>
        ${XR_PROGRAM_IMPORTED_CALLABLE_GENERATED_C}
    DEPENDS test_xr_program_imported_callable
    COMMENT "Generating source-backed imported callable canonical-program C fixture"
    VERBATIM)
set_source_files_properties(${XR_PROGRAM_IMPORTED_CALLABLE_GENERATED_C}
    PROPERTIES GENERATED TRUE)
add_executable(test_xr_program_imported_callable_native
    ${XR_PROGRAM_IMPORTED_CALLABLE_GENERATED_C})
xr_enable_pure_aot_symbol_map(test_xr_program_imported_callable_native)
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
add_xray_unit_test(test_xi_compare ir/test_xi_compare.c)
add_xray_bootstrap_unit_test(test_xi_cgen ir/test_xi_cgen.c)
add_test(NAME test_xi_cgen_multimodule
    COMMAND ${CMAKE_COMMAND} -E env XRAY_TEST_FILTER=cgen_multimodule
        $<TARGET_FILE:test_xi_cgen>)
set_tests_properties(test_xi_cgen_multimodule PROPERTIES
    LABELS "aot;cgen;multi-module;reachability")
add_test(NAME test_xi_cgen_concat_authority
    COMMAND $<TARGET_FILE:test_xi_cgen> concat-authority)
set_tests_properties(test_xi_cgen_concat_authority PROPERTIES
    LABELS "aot;cgen;string;ownership")
add_test(NAME test_xi_cgen_native_direct_authority
    COMMAND $<TARGET_FILE:test_xi_cgen> native-direct-authority)
set_tests_properties(test_xi_cgen_native_direct_authority PROPERTIES
    LABELS "aot;cgen;native;ownership;reachability")
add_test(NAME test_xi_cgen_structural_root COMMAND test_xi_cgen structural-root)
set_tests_properties(test_xi_cgen_structural_root PROPERTIES
    LABELS "aot;cgen;ownership;tuple")
add_test(NAME test_xi_cgen_parameter_snapshot
    COMMAND test_xi_cgen --case cgen_direct_suspend_method_call_propagates_cps)
set_tests_properties(test_xi_cgen_parameter_snapshot PROPERTIES
    LABELS "aot;cgen;memory;ownership;coroutine")
add_test(NAME test_xi_cgen_module_struct_copy COMMAND test_xi_cgen module-struct-copy)
set_tests_properties(test_xi_cgen_module_struct_copy PROPERTIES
    LABELS "aot;cgen;ownership;struct")
add_test(NAME test_xi_cgen_aggregate_snapshot
    COMMAND test_xi_cgen --case cgen_local_value_struct_copy_consumes_named_aggregate_emission)
set_tests_properties(test_xi_cgen_aggregate_snapshot PROPERTIES
    LABELS "aot;cgen;memory;ownership;struct")
add_test(NAME test_xi_cgen_executable_recipes
    COMMAND $<TARGET_FILE:test_xi_cgen> executable-recipes)
set_tests_properties(test_xi_cgen_executable_recipes PROPERTIES
    LABELS "aot;cgen;string;ownership;reachability")
add_test(NAME test_xi_cgen_span_box_carrier
    COMMAND ${CMAKE_COMMAND} -E env XRAY_TEST_FILTER=cgen_rep_identical_span_box
        $<TARGET_FILE:test_xi_cgen>)
set_tests_properties(test_xi_cgen_span_box_carrier PROPERTIES
    LABELS "aot;cgen;ownership;representation")
add_test(NAME test_xi_cgen_optional_injection
    COMMAND $<TARGET_FILE:test_xi_cgen> optional-injection-emission)
add_test(NAME test_xi_cgen_optional_storage
    COMMAND $<TARGET_FILE:test_xi_cgen> optional-storage-emission)
set_tests_properties(test_xi_cgen_optional_injection PROPERTIES
    LABELS "aot;cgen;optional;stdlib")
set_tests_properties(test_xi_cgen_optional_storage PROPERTIES
    LABELS "aot;cgen;optional")
set(XR_OPTIONAL_STORAGE_SOURCE_PREFIX
    ${CMAKE_CURRENT_BINARY_DIR}/generated/optional_storage)
add_custom_command(
    OUTPUT ${XR_OPTIONAL_STORAGE_SOURCE_PREFIX}.0.c
           ${XR_OPTIONAL_STORAGE_SOURCE_PREFIX}.1.c
           ${XR_OPTIONAL_STORAGE_SOURCE_PREFIX}.2.c
           ${XR_OPTIONAL_STORAGE_SOURCE_PREFIX}.3.c
           ${XR_OPTIONAL_STORAGE_SOURCE_PREFIX}.4.c
           ${XR_OPTIONAL_STORAGE_SOURCE_PREFIX}.5.c
           ${XR_OPTIONAL_STORAGE_SOURCE_PREFIX}.6.c
           ${XR_OPTIONAL_STORAGE_SOURCE_PREFIX}.7.c
           ${XR_OPTIONAL_STORAGE_SOURCE_PREFIX}.8.c
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND $<TARGET_FILE:test_xi_cgen> optional-storage-emission
            ${XR_OPTIONAL_STORAGE_SOURCE_PREFIX}
    DEPENDS test_xi_cgen
    COMMENT "Generating verified optional scalar C fixtures"
    VERBATIM)
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
    target_link_libraries(${XR_OPTIONAL_NATIVE_TARGET} PRIVATE xray_core)
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
    set(XR_NATIVE_GENERATOR test_xi_cgen)
    set(XR_NATIVE_GENERATE_COMMAND $<TARGET_FILE:test_xi_cgen> ${XR_NATIVE_SELECTOR} ${XR_NATIVE_GENERATED_C})
    if(XR_NATIVE_FIXTURE MATCHES "^(module_ref|entry_closure|native_storage)$")
        set(XR_NATIVE_TARGET test_xaot_${XR_NATIVE_FIXTURE}_native)
        set(XR_NATIVE_GENERATOR test_xaot_driver)
        set(XR_NATIVE_GENERATE_COMMAND ${CMAKE_COMMAND} -E env
            XRAY_TEST_FILTER=${XR_NATIVE_FIXTURE} XRAY_TEST_OUTPUT_C=${XR_NATIVE_GENERATED_C}
            $<TARGET_FILE:test_xaot_driver>)
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
    target_link_libraries(${XR_NATIVE_TARGET} PRIVATE xray_core)
    target_include_directories(${XR_NATIVE_TARGET} SYSTEM PRIVATE
        ${CMAKE_SOURCE_DIR}/src/aot ${CMAKE_SOURCE_DIR}/include)
    set_target_properties(${XR_NATIVE_TARGET} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED TRUE C_EXTENSIONS FALSE)
    xr_enable_pure_aot_symbol_map(${XR_NATIVE_TARGET})
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
add_test(NAME test_xi_cgen_iterator_rune_nth_emission
    COMMAND $<TARGET_FILE:test_xi_cgen> iterator-rune-nth-emission)
target_sources(test_xi_cgen PRIVATE plan/target_profile_test_fixture.c)
target_compile_definitions(test_xi_cgen PRIVATE
    XRAY_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
if(MSVC OR CMAKE_C_SIMULATE_ID STREQUAL "MSVC")
    # This exhaustive generator test recursively lowers hundreds of fixtures.
    # MSVC ASan expands the call frames beyond PE/COFF's 1 MiB default stack.
    target_link_options(test_xi_cgen PRIVATE "LINKER:/STACK:8388608")
endif()
add_xray_unit_test(test_xi_lower_hardfail ir/test_xi_lower_hardfail.c)
add_xray_unit_test(test_xi_stage ir/test_xi_stage.c)
add_xray_unit_test(test_xi_evidence ir/test_xi_evidence.c)
add_xray_unit_test(test_xi_intrinsic ir/test_xi_intrinsic.c)
add_xray_unit_test(test_xi_effect ir/test_xi_effect.c)
add_xray_bootstrap_unit_test(test_xi_escape ir/test_xi_escape.c)
add_xray_unit_test(test_xi_own ir/test_xi_own.c)
add_xray_unit_test(test_xi_arc_verify ir/test_xi_arc_verify.c)
add_xray_unit_test(test_xi_source_move_verify ir/test_xi_source_move_verify.c)
add_xray_bootstrap_unit_test(test_semantic_plan plan/test_semantic_plan.c)
add_test(NAME test_semantic_runtime_task_ownership
    COMMAND $<TARGET_FILE:test_semantic_plan> runtime-task-ownership)
set_tests_properties(test_semantic_runtime_task_ownership PROPERTIES
    LABELS "semantic;ownership;coroutine" TIMEOUT 120)

add_test(NAME test_semantic_raw_slice COMMAND $<TARGET_FILE:test_semantic_plan> raw-slice)
set_tests_properties(test_semantic_raw_slice PROPERTIES LABELS "semantic;ownership;mutation")
add_test(NAME test_semantic_zero_operand_metadata
    COMMAND $<TARGET_FILE:test_semantic_plan> zero-operand-metadata)
set_tests_properties(test_semantic_zero_operand_metadata PROPERTIES
    LABELS "semantic;memory-safety;metadata")
add_test(NAME test_semantic_dependency_method_authority
    COMMAND $<TARGET_FILE:test_semantic_plan> source-dependency-method-authority)
set_tests_properties(test_semantic_dependency_method_authority PROPERTIES
    LABELS "semantic;typed-plan;module-graph;coroutine")
set(XR_NATIVE_SOURCE_HOST_ARGS --host-compiler ${CMAKE_C_COMPILER})
if(MSVC)
    list(APPEND XR_NATIVE_SOURCE_HOST_ARGS --msvc)
endif()
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    list(APPEND XR_NATIVE_SOURCE_HOST_ARGS --msvc-atomics)
endif()
add_test(NAME test_native_string_search_authority
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/cli/test_native_string_search.py
        --compiler $<TARGET_FILE:xray_stdlib_bcgen> ${XR_NATIVE_SOURCE_HOST_ARGS})
set_tests_properties(test_native_string_search_authority PROPERTIES
    LABELS "aot;generated-c;runtime-method;integration"
    TIMEOUT 240)
add_test(NAME test_semantic_channel_type_identity
    COMMAND $<TARGET_FILE:test_semantic_plan> channel-type-identity)
set_tests_properties(test_semantic_channel_type_identity PROPERTIES LABELS "semantic;channel;ownership")
add_test(NAME test_native_channel_field_authority
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/cli/test_native_channel_field.py
        --compiler $<TARGET_FILE:xray_stdlib_bcgen> ${XR_NATIVE_SOURCE_HOST_ARGS})
set_tests_properties(test_native_channel_field_authority PROPERTIES
    LABELS "aot;generated-c;channel;ownership;integration" TIMEOUT 240)
add_test(NAME test_semantic_nullable_reference_phi
    COMMAND $<TARGET_FILE:test_semantic_plan> nullable-reference-phi)
set_tests_properties(test_semantic_nullable_reference_phi PROPERTIES LABELS "semantic;ownership")
add_test(NAME test_semantic_external_field_type_identity
    COMMAND $<TARGET_FILE:test_semantic_plan> external-field-type-identity)
set_tests_properties(test_semantic_external_field_type_identity PROPERTIES LABELS "semantic;class;ownership")
add_test(NAME test_native_array_pop_authority
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/cli/test_native_array_pop.py
        --compiler $<TARGET_FILE:xray_stdlib_bcgen> ${XR_NATIVE_SOURCE_HOST_ARGS})
set_tests_properties(test_native_array_pop_authority PROPERTIES
    LABELS "aot;generated-c;array;ownership;integration" TIMEOUT 240)
add_test(NAME test_native_json_codec_authority
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/cli/test_native_json_codec.py
        --compiler $<TARGET_FILE:xray_stdlib_bcgen> ${XR_NATIVE_SOURCE_HOST_ARGS})
set_tests_properties(test_native_json_codec_authority PROPERTIES
    LABELS "aot;generated-c;json;ownership;integration" TIMEOUT 240)
add_test(NAME test_native_coroutine_method_authority
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/cli/test_native_coroutine_method.py
        --compiler $<TARGET_FILE:xray_stdlib_bcgen> ${XR_NATIVE_SOURCE_HOST_ARGS})
set_tests_properties(test_native_coroutine_method_authority PROPERTIES
    LABELS "aot;generated-c;coroutine;integration" TIMEOUT 240)
add_test(NAME test_native_tuple_boundary_authority
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/cli/test_native_tuple_boundary.py
        --compiler $<TARGET_FILE:xray_stdlib_bcgen> ${XR_NATIVE_SOURCE_HOST_ARGS})
set_tests_properties(test_native_tuple_boundary_authority PROPERTIES
    LABELS "aot;generated-c;tuple;ownership;integration" TIMEOUT 240)
add_test(NAME test_native_tuple_array_authority
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/cli/test_native_tuple_array.py
        --compiler $<TARGET_FILE:xray_stdlib_bcgen> ${XR_NATIVE_SOURCE_HOST_ARGS})
set_tests_properties(test_native_tuple_array_authority PROPERTIES
    LABELS "aot;generated-c;array;ownership;integration" TIMEOUT 240)
add_test(NAME test_native_string_concat_authority
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/cli/test_native_string_concat.py
        --compiler $<TARGET_FILE:xray_stdlib_bcgen> ${XR_NATIVE_SOURCE_HOST_ARGS})
set_tests_properties(test_native_string_concat_authority PROPERTIES
    LABELS "aot;generated-c;string;ownership;integration" TIMEOUT 120)
add_test(NAME test_native_execution_closure_authority
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/cli/test_native_execution_closure.py
        --compiler $<TARGET_FILE:xray_stdlib_bcgen> ${XR_NATIVE_SOURCE_HOST_ARGS})
set_tests_properties(test_native_execution_closure_authority PROPERTIES
    LABELS "aot;generated-c;closure;integration" TIMEOUT 240)
add_test(NAME test_native_static_method_authority
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/cli/test_native_static_method.py
        --compiler $<TARGET_FILE:xray_stdlib_bcgen> ${XR_NATIVE_SOURCE_HOST_ARGS})
set_tests_properties(test_native_static_method_authority PROPERTIES
    LABELS "aot;generated-c;class;ownership;integration" TIMEOUT 240)
add_test(NAME test_native_move_receiver_authority
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/cli/test_native_move_receiver.py
        --compiler $<TARGET_FILE:xray_stdlib_bcgen> ${XR_NATIVE_SOURCE_HOST_ARGS})
set_tests_properties(test_native_move_receiver_authority PROPERTIES
    LABELS "aot;generated-c;class;ownership;integration" TIMEOUT 240)
add_test(NAME test_target_const_owner_transfer
    COMMAND $<TARGET_FILE:test_target_plan> const-owner-transfer)
set_tests_properties(test_target_const_owner_transfer PROPERTIES
    LABELS "typed-plan;array;ownership" TIMEOUT 60)
add_test(NAME test_enum_namespace_execution
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/cli/run_enum_namespace_tests.py
        --compiler $<TARGET_FILE:xray>)
set_tests_properties(test_enum_namespace_execution PROPERTIES
    LABELS "vm;aot;enum;integration" TIMEOUT 300)
add_test(NAME test_cgen_go_managed
    COMMAND $<TARGET_FILE:test_xi_cgen> go-managed)
set_tests_properties(test_cgen_go_managed PROPERTIES
    LABELS "aot;coroutine;ownership" TIMEOUT 120)
foreach(channel_case IN ITEMS
        cgen_channel_closed_property_rejects_call
        cgen_array_shift_managed_execution
        cgen_direct_move_array_execution
        cgen_nullable_unit_enum_execution
        cgen_nullable_unit_enum_compare_execution
        cgen_string_copy_execution
        cgen_ref_slice_copy_execution
        cgen_task_cancel_blocked_execution
        cgen_coro_task_status_uses_native_enum_status
        cgen_task_poll_pending_and_complete
        cgen_task_await_timeout_then_complete
        cgen_coro_await_timeout_passes_deadline
        cgen_channel_recv_timeout_execution
        cgen_coro_unit_match_send_omits_void_phi
        cgen_blocking_channel_rendezvous_result
        cgen_coro_syncs_helper_result_debug_source_vars
        cgen_channel_receive_payload_execution
        cgen_descriptor_scalar_channel_try_recv_returns_recv_enum
        cgen_builtin_unit_enum_member_authority
        cgen_descriptor_scalar_channel_try_send_uses_typed_sync_bridge
        cgen_descriptor_tagged_channel_try_send_normalizes_runtime_envelope
        cgen_sync_go_channel_try_methods_use_aot_helpers)
    add_test(NAME test_${channel_case}
        COMMAND $<TARGET_FILE:test_xi_cgen> --case ${channel_case})
    set_tests_properties(test_${channel_case} PROPERTIES
        LABELS "aot;channel;ownership" TIMEOUT 120)
endforeach()
# Complete programs lowered by test_xi_cgen, strictly compiled, linked with the
# same AOT core archive the CLI links, executed, and compared byte for byte
# with independent answers kept in the runner.
foreach(execution_case IN ITEMS
        cgen_nullable_unit_enum_compare_execution
        cgen_nullable_unit_enum_execution
        cgen_ref_slice_copy_execution
        cgen_string_copy_execution
        cgen_direct_move_array_execution)
    add_test(NAME test_run_${execution_case}
        COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/cli/run_cgen_execution.py
            --cgen $<TARGET_FILE:test_xi_cgen> --case ${execution_case}
            --aot-core $<TARGET_FILE:xray_aot_core> ${XR_NATIVE_SOURCE_HOST_ARGS})
    set_tests_properties(test_run_${execution_case} PROPERTIES
        LABELS "aot;generated-c;execution;integration" TIMEOUT 240)
endforeach()
add_test(NAME test_native_channel_recv_authority
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/cli/test_native_channel_recv.py
        --compiler $<TARGET_FILE:xray_stdlib_bcgen> ${XR_NATIVE_SOURCE_HOST_ARGS})
set_tests_properties(test_native_channel_recv_authority PROPERTIES
    LABELS "aot;generated-c;channel;ownership;integration" TIMEOUT 240)
add_test(NAME test_native_channel_send_authority
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/cli/test_native_channel_send.py
        --compiler $<TARGET_FILE:xray_stdlib_bcgen> ${XR_NATIVE_SOURCE_HOST_ARGS})
set_tests_properties(test_native_channel_send_authority PROPERTIES
    LABELS "aot;generated-c;channel;ownership;integration" TIMEOUT 240)
add_test(NAME test_native_go_managed_authority
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/cli/test_native_go_managed.py
        --compiler $<TARGET_FILE:xray_stdlib_bcgen> ${XR_NATIVE_SOURCE_HOST_ARGS})
set_tests_properties(test_native_go_managed_authority PROPERTIES
    LABELS "aot;generated-c;coroutine;ownership;integration" TIMEOUT 240)
add_test(NAME test_native_captured_slice_authority
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/cli/test_native_captured_slice.py
        --compiler $<TARGET_FILE:xray_stdlib_bcgen> ${XR_NATIVE_SOURCE_HOST_ARGS})
set_tests_properties(test_native_captured_slice_authority PROPERTIES
    LABELS "aot;generated-c;ownership;integration" TIMEOUT 240)
add_test(NAME test_native_structural_object_carrier_authority
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/cli/test_native_structural_object_carrier.py
        --compiler $<TARGET_FILE:xray_stdlib_bcgen> ${XR_NATIVE_SOURCE_HOST_ARGS})
set_tests_properties(test_native_structural_object_carrier_authority PROPERTIES
    LABELS "aot;generated-c;array;ownership;integration" TIMEOUT 240)
add_test(NAME test_native_class_coalesce_authority
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/cli/test_native_class_coalesce.py
        --compiler $<TARGET_FILE:xray_stdlib_bcgen> ${XR_NATIVE_SOURCE_HOST_ARGS})
set_tests_properties(test_native_class_coalesce_authority PROPERTIES
    LABELS "aot;generated-c;class;ownership;integration" TIMEOUT 240)
add_test(NAME test_native_ref_array_authority
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/cli/test_native_ref_array.py
        --compiler $<TARGET_FILE:xray_stdlib_bcgen> ${XR_NATIVE_SOURCE_HOST_ARGS})
set_tests_properties(test_native_ref_array_authority PROPERTIES
    LABELS "aot;generated-c;array;ownership;integration" TIMEOUT 240)
add_test(NAME test_native_ref_slice_authority
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/cli/test_native_ref_slice.py
        --compiler $<TARGET_FILE:xray_stdlib_bcgen> ${XR_NATIVE_SOURCE_HOST_ARGS})
set_tests_properties(test_native_ref_slice_authority PROPERTIES
    LABELS "aot;generated-c;slice;ownership;integration" TIMEOUT 240)
add_test(NAME test_native_string_copy_authority
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/cli/test_native_string_copy.py
        --compiler $<TARGET_FILE:xray_stdlib_bcgen> ${XR_NATIVE_SOURCE_HOST_ARGS})
set_tests_properties(test_native_string_copy_authority PROPERTIES
    LABELS "aot;generated-c;string;ownership;integration" TIMEOUT 240)
add_test(NAME test_native_nullable_enum_authority
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/cli/test_native_nullable_enum.py
        --compiler $<TARGET_FILE:xray_stdlib_bcgen> ${XR_NATIVE_SOURCE_HOST_ARGS})
set_tests_properties(test_native_nullable_enum_authority PROPERTIES
    LABELS "aot;generated-c;enum;integration" TIMEOUT 240)
add_test(NAME test_native_enum_namespace_authority
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/cli/test_native_enum_namespace.py
        --compiler $<TARGET_FILE:xray_stdlib_bcgen> ${XR_NATIVE_SOURCE_HOST_ARGS})
set_tests_properties(test_native_enum_namespace_authority PROPERTIES
    LABELS "aot;generated-c;enum;integration" TIMEOUT 240)
add_test(NAME test_native_module_const_authority
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/cli/test_native_module_const.py
        --compiler $<TARGET_FILE:xray_stdlib_bcgen> ${XR_NATIVE_SOURCE_HOST_ARGS})
set_tests_properties(test_native_module_const_authority PROPERTIES
    LABELS "aot;generated-c;array;ownership;integration" TIMEOUT 240)
add_test(NAME test_target_array_owned_string_result
    COMMAND $<TARGET_FILE:test_target_plan> array-owned-string-result)
set_tests_properties(test_target_array_owned_string_result PROPERTIES
    LABELS "typed-plan;array;ownership" TIMEOUT 60)
add_test(NAME test_native_array_string_authority
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/cli/test_native_array_string.py
        --compiler $<TARGET_FILE:xray_stdlib_bcgen> ${XR_NATIVE_SOURCE_HOST_ARGS})
set_tests_properties(test_native_array_string_authority PROPERTIES
    LABELS "aot;generated-c;array;ownership;integration" TIMEOUT 240)
add_test(NAME test_native_atomic_authority
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/cli/test_native_atomic.py
        --compiler $<TARGET_FILE:xray_stdlib_bcgen> ${XR_NATIVE_SOURCE_HOST_ARGS})
set_tests_properties(test_native_atomic_authority PROPERTIES
    LABELS "aot;generated-c;atomic;ownership;integration" TIMEOUT 240)
add_test(NAME test_native_external_field_authority
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/cli/test_native_external_field.py
        --compiler $<TARGET_FILE:xray_stdlib_bcgen> ${XR_NATIVE_SOURCE_HOST_ARGS})
set_tests_properties(test_native_external_field_authority PROPERTIES
    LABELS "aot;generated-c;class;ownership;integration" TIMEOUT 240)
add_test(NAME test_semantic_builtin_runtime_method_authority
    COMMAND $<TARGET_FILE:test_semantic_plan> builtin-runtime-method-authority)
set_tests_properties(test_semantic_builtin_runtime_method_authority PROPERTIES
    LABELS "semantic;typed-plan;runtime-method")
add_xray_unit_test(test_program_semantic_closure plan/test_program_semantic_closure.c)
add_test(NAME test_program_semantic_call_locator
    COMMAND $<TARGET_FILE:test_program_semantic_closure>)
set_tests_properties(test_program_semantic_call_locator PROPERTIES
    LABELS "semantic;program-closure;task-283")
add_xray_unit_test(test_xi_program_semantic ir/test_xi_program_semantic.c)
set_tests_properties(test_xi_program_semantic PROPERTIES
    LABELS "ir;semantic;target-machine;ownership;task-283")
set(XR_LEAF_PRODUCT_NATIVE_SOURCE
    ${CMAKE_CURRENT_BINARY_DIR}/generated/leaf_product_native.c)
add_custom_command(
    OUTPUT ${XR_LEAF_PRODUCT_NATIVE_SOURCE}
    COMMAND ${CMAKE_COMMAND} -E make_directory
            ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND ${CMAKE_COMMAND} -E env
            XR_LEAF_PRODUCT_NATIVE_C_OUTPUT=${XR_LEAF_PRODUCT_NATIVE_SOURCE}
            $<TARGET_FILE:test_xi_program_semantic>
    DEPENDS test_xi_program_semantic
    COMMENT "Generating verified tuple6 TargetPlan native C fixture"
    VERBATIM)
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
set(XR_I64_OVERFLOW_NATIVE_SOURCE
    ${CMAKE_CURRENT_BINARY_DIR}/generated/i64_overflow_native.c)
add_custom_command(
    OUTPUT ${XR_I64_OVERFLOW_NATIVE_SOURCE}
    COMMAND ${CMAKE_COMMAND} -E make_directory
            ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND ${CMAKE_COMMAND} -E env
            XR_I64_OVERFLOW_NATIVE_C_OUTPUT=${XR_I64_OVERFLOW_NATIVE_SOURCE}
            $<TARGET_FILE:test_xi_program_semantic>
    DEPENDS test_xi_program_semantic
    COMMENT "Generating verified i64 overflow TargetPlan native C fixture"
    VERBATIM)
set_source_files_properties(${XR_I64_OVERFLOW_NATIVE_SOURCE}
    PROPERTIES GENERATED TRUE)
add_executable(test_i64_overflow_native
    ${XR_I64_OVERFLOW_NATIVE_SOURCE})
target_link_libraries(test_i64_overflow_native PRIVATE xray_core)
target_include_directories(test_i64_overflow_native SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/src/aot
    ${CMAKE_SOURCE_DIR}/include)
set_target_properties(test_i64_overflow_native PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED TRUE)
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC"
        OR (CMAKE_C_COMPILER_ID STREQUAL "Clang"
            AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    # Runtime implementation fragments are included by xrt.h; MSVC diagnoses
    # two deliberately unreachable generic fallbacks in those external headers.
    target_compile_options(test_i64_overflow_native PRIVATE /W4 /WX /wd4702)
else()
    target_compile_options(test_i64_overflow_native PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_i64_overflow_native
    COMMAND $<TARGET_FILE:test_i64_overflow_native>)
set_tests_properties(test_i64_overflow_native PROPERTIES
    LABELS "aot;cgen;native;target-machine;task-283")
add_executable(test_semantic_type_admission plan/test_semantic_type_admission.c)
target_include_directories(test_semantic_type_admission PRIVATE ${XRAY_COMMON_INCLUDES})
add_test(NAME test_semantic_type_admission COMMAND $<TARGET_FILE:test_semantic_type_admission>)
set(XR_TARGET_PROFILE_TEST_FIXTURE plan/target_profile_test_fixture.c)
add_xray_unit_test(test_target_profile plan/test_target_profile.c
    ${XR_TARGET_PROFILE_TEST_FIXTURE})
add_executable(test_xr_execution execution/test_xr_execution.c
    ${XR_TARGET_PROFILE_TEST_FIXTURE})
if(WIN32)
    target_sources(test_xr_execution PRIVATE ${CMAKE_SOURCE_DIR}/src/os/win/thread_win.c)
else()
    target_sources(test_xr_execution PRIVATE ${CMAKE_SOURCE_DIR}/src/os/unix/thread_unix.c)
endif()
target_include_directories(test_xr_execution PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(test_xr_execution PRIVATE xr_program_test_core xray_program_vm_runtime)
add_test(NAME test_xr_execution COMMAND $<TARGET_FILE:test_xr_execution>)
set_tests_properties(test_xr_execution PROPERTIES
    LABELS "unit;execution;target-machine;provider;generation;task-298")
add_executable(test_xr_typed_provider execution/test_xr_typed_provider.c
    ${XR_TARGET_PROFILE_TEST_FIXTURE})
target_include_directories(test_xr_typed_provider PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(test_xr_typed_provider PRIVATE xr_program_test_core xray_program_vm_runtime)
add_test(NAME test_xr_typed_provider COMMAND $<TARGET_FILE:test_xr_typed_provider>)
set_tests_properties(test_xr_typed_provider PROPERTIES LABELS "unit;execution;provider;ownership")
add_library(xr_execution_allocation_probe OBJECT ${CMAKE_SOURCE_DIR}/src/execution/xr_execution.c)
target_include_directories(xr_execution_allocation_probe PRIVATE ${XRAY_COMMON_INCLUDES})
if(MSVC OR CMAKE_C_SIMULATE_ID STREQUAL "MSVC")
    target_compile_options(xr_execution_allocation_probe PRIVATE
        /W4 /WX "/FI${CMAKE_CURRENT_SOURCE_DIR}/program/xr_program_allocation_probe.h")
else()
    target_compile_options(xr_execution_allocation_probe PRIVATE
        -Wall -Wextra -Werror -include "${CMAKE_CURRENT_SOURCE_DIR}/program/xr_program_allocation_probe.h")
endif()
add_executable(test_xr_execution_allocations execution/test_xr_execution_allocations.c
    ${XR_TARGET_PROFILE_TEST_FIXTURE} $<TARGET_OBJECTS:xr_execution_allocation_probe>)
target_include_directories(test_xr_execution_allocations PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(test_xr_execution_allocations PRIVATE xr_program_test_core xray_program_vm_runtime)
add_test(NAME test_xr_execution_allocations COMMAND $<TARGET_FILE:test_xr_execution_allocations>)
set_tests_properties(test_xr_execution_allocations PROPERTIES
    LABELS "unit;execution;allocation-failure;ownership;generation")
add_executable(test_xr_typed_provider_allocations execution/test_xr_typed_provider.c
    ${XR_TARGET_PROFILE_TEST_FIXTURE} $<TARGET_OBJECTS:xr_execution_allocation_probe>
    $<TARGET_OBJECTS:xr_program_vm_allocation_probe>)
target_compile_definitions(test_xr_typed_provider_allocations PRIVATE XR_TYPED_PROVIDER_ALLOCATION_TEST=1)
target_include_directories(test_xr_typed_provider_allocations PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(test_xr_typed_provider_allocations PRIVATE xr_program_test_core xray_program_vm_runtime)
add_test(NAME test_xr_typed_provider_allocations COMMAND $<TARGET_FILE:test_xr_typed_provider_allocations>)
set_tests_properties(test_xr_typed_provider_allocations PROPERTIES LABELS "unit;execution;provider;ownership;allocation-failure")
add_xray_unit_test(test_xr_boundary_materialization
    execution/test_xr_boundary_materialization.c
    ${XR_TARGET_PROFILE_TEST_FIXTURE})
set_tests_properties(test_xr_boundary_materialization PROPERTIES
    LABELS "unit;execution;boundary-abi;aggregate;variant;task-298;task-301")
add_executable(test_xr_program_vm vm/test_xr_program_vm.c
    ${XR_TARGET_PROFILE_TEST_FIXTURE})
if(WIN32)
    target_sources(test_xr_program_vm PRIVATE
        ${CMAKE_SOURCE_DIR}/src/os/win/thread_win.c)
else()
    target_sources(test_xr_program_vm PRIVATE
        ${CMAKE_SOURCE_DIR}/src/os/unix/thread_unix.c)
endif()
target_compile_definitions(test_xr_program_vm PRIVATE XR_TEST_HAS_PROCESS_SHUTDOWN=1)
target_include_directories(test_xr_program_vm PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_xr_program_vm PRIVATE
    xr_program_test_core xray_program_vm_runtime)
add_test(NAME test_xr_program_vm COMMAND $<TARGET_FILE:test_xr_program_vm>)
set_tests_properties(test_xr_program_vm PROPERTIES
    LABELS "unit;vm;canonical-program;differential;target-machine;generation;task-299")

# This opt-in tool refreshes the checked-in runtime fixture when the canonical
# artifact schema changes. It is intentionally excluded from normal builds and
# is not a dependency of the runtime-only consumer below.
add_executable(xr_program_vm_fixture_writer EXCLUDE_FROM_ALL
    program/xr_program_vm_fixture_writer.c)
target_include_directories(xr_program_vm_fixture_writer PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xr_program_vm_fixture_writer PRIVATE xr_program_test_core)

add_executable(test_xr_program_vm_runtime
    vm/test_xr_program_vm_runtime.c
    ${XR_TARGET_PROFILE_TEST_FIXTURE})
target_compile_definitions(test_xr_program_vm_runtime PRIVATE
    XR_TEST_HAS_PROCESS_SHUTDOWN=1)
target_include_directories(test_xr_program_vm_runtime PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_xr_program_vm_runtime PRIVATE
    xray_program_vm_runtime)
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC"
        OR (CMAKE_C_COMPILER_ID STREQUAL "Clang"
            AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    target_compile_options(xr_program_vm_fixture_writer PRIVATE /W4 /WX)
    target_compile_options(test_xr_program_vm PRIVATE /W4 /WX)
    target_compile_options(test_xr_program_vm PRIVATE /wd4101 /clang:-Wno-unused-variable)
    target_compile_options(test_xr_program_vm_runtime PRIVATE /W4 /WX)
else()
    target_compile_options(xr_program_vm_fixture_writer PRIVATE -Wall -Wextra -Werror)
    target_compile_options(test_xr_program_vm PRIVATE
        -Wall -Wextra -Werror -Wno-unused-variable)
    target_compile_options(test_xr_program_vm_runtime PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xr_program_vm_runtime
    COMMAND $<TARGET_FILE:test_xr_program_vm_runtime>)
set_tests_properties(test_xr_program_vm_runtime PROPERTIES
    LABELS "unit;vm;canonical-program;runtime-only;embedder;task-299")

add_executable(test_xr_program_aot
    aot/test_xr_program_aot.c
    ${CMAKE_SOURCE_DIR}/src/runtime/abi/xr_runtime_target_profile.c
    ${CMAKE_SOURCE_DIR}/src/runtime/abi/xr_runtime_target_authority.c
    ${CMAKE_SOURCE_DIR}/src/runtime/abi/xr_stdlib_provider_projection.c
    ${XR_TARGET_PROFILE_TEST_FIXTURE})
target_compile_definitions(test_xr_program_aot PRIVATE XR_TEST_HAS_PROCESS_SHUTDOWN=1)
target_include_directories(test_xr_program_aot PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_xr_program_aot PRIVATE
    xr_program_test_core xray_program_vm_runtime xray_program_aot_compiler)
add_test(NAME test_xr_program_aot COMMAND $<TARGET_FILE:test_xr_program_aot>)
add_dependencies(test_xr_program_aot xray_program_aot_compiler)
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC"
        OR (CMAKE_C_COMPILER_ID STREQUAL "Clang"
            AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    target_compile_options(test_xr_program_aot PRIVATE /W4 /WX)
else()
    target_compile_options(test_xr_program_aot PRIVATE -Wall -Wextra -Werror)
endif()
set_tests_properties(test_xr_program_aot PROPERTIES
    LABELS "unit;aot;canonical-program;backendir;program-binding;task-300")

# The manifest keeps this gate in exact expected-unsupported mode until both
# backend owners explicitly activate their probe protocol.  Once active, the
# same CTest invokes these existing executables and compares all four executor
# records with the frozen Reference oracle. The AOT generated-C mode owns a
# real strict-C11 compile and host-run; emitting or scanning C alone cannot
# produce a qualifying record.
add_test(NAME xr_program_h2_backend_differential
    COMMAND ${XRAY_PYTHON}
        ${CMAKE_SOURCE_DIR}/scripts/check_xr_program_h2_backend_differential.py
        --vm $<TARGET_FILE:test_xr_program_vm>
        --aot $<TARGET_FILE:test_xr_program_aot>)
set_tests_properties(xr_program_h2_backend_differential PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    LABELS "unit;canonical-program;differential;vm;aot;generated-c;native;task-307"
    TIMEOUT 180 PROCESSORS 1)

add_executable(xr_program_allocation_fixture_writer
    aot/xr_program_allocation_fixture_writer.c
    ${XR_TARGET_PROFILE_TEST_FIXTURE})
target_include_directories(xr_program_allocation_fixture_writer PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xr_program_allocation_fixture_writer PRIVATE
    xr_program_test_core xray_program_vm_runtime xray_program_aot_compiler)
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC"
        OR (CMAKE_C_COMPILER_ID STREQUAL "Clang"
            AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    target_compile_options(xr_program_allocation_fixture_writer PRIVATE /W4 /WX)
else()
    target_compile_options(xr_program_allocation_fixture_writer PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME xr_program_allocation_native_self_test
    COMMAND ${XRAY_PYTHON} -m unittest discover
        -s ${CMAKE_SOURCE_DIR}/tests/lib/tests -p test_program_allocation_native.py)
set_tests_properties(xr_program_allocation_native_self_test PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    LABELS "unit;aot;generated-c;allocation;self-test")
if(WIN32)
    add_custom_target(test_xr_program_allocation_aot_native
        DEPENDS xr_program_allocation_fixture_writer)
    add_test(NAME test_xr_program_allocation_aot_native
        COMMAND ${XRAY_PYTHON}
            ${CMAKE_SOURCE_DIR}/scripts/check_xr_program_allocation_native.py
            --writer $<TARGET_FILE:xr_program_allocation_fixture_writer>)
    set_tests_properties(test_xr_program_allocation_aot_native PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        LABELS "unit;aot;generated-c;allocation;msvc;portable-c11"
        RESOURCE_LOCK xray_native_toolchain
        TIMEOUT 120 PROCESSORS 1)
endif()

add_executable(xr_program_coroutine_graph_fixture_writer
    aot/xr_program_coroutine_graph_fixture_writer.c
    ${XR_TARGET_PROFILE_TEST_FIXTURE})
target_include_directories(xr_program_coroutine_graph_fixture_writer PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xr_program_coroutine_graph_fixture_writer PRIVATE
    xr_program_test_core xray_program_aot_compiler)
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC"
        OR (CMAKE_C_COMPILER_ID STREQUAL "Clang"
            AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    target_compile_options(xr_program_coroutine_graph_fixture_writer PRIVATE /W4 /WX)
else()
    target_compile_options(xr_program_coroutine_graph_fixture_writer PRIVATE
        -Wall -Wextra -Werror)
endif()
add_test(NAME xr_program_coroutine_graph_native_self_test
    COMMAND ${XRAY_PYTHON} -m unittest discover
        -s ${CMAKE_SOURCE_DIR}/tests/lib/tests -p test_program_coroutine_graph_native.py)
set_tests_properties(xr_program_coroutine_graph_native_self_test PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    LABELS "unit;aot;generated-c;coroutine;self-test")
if(WIN32)
    add_custom_target(test_xr_program_coroutine_graph_aot_native
        DEPENDS xr_program_coroutine_graph_fixture_writer)
    add_test(NAME test_xr_program_coroutine_graph_aot_native
        COMMAND ${XRAY_PYTHON}
            ${CMAKE_SOURCE_DIR}/scripts/check_xr_program_coroutine_graph_native.py
            --writer $<TARGET_FILE:xr_program_coroutine_graph_fixture_writer>)
    set_tests_properties(test_xr_program_coroutine_graph_aot_native PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        LABELS "unit;aot;generated-c;coroutine;msvc;portable-c11"
        RESOURCE_LOCK xray_native_toolchain
        TIMEOUT 120 PROCESSORS 1)
endif()

set(XR_PROGRAM_AOT_FIXTURE_DIR
    ${CMAKE_CURRENT_BINARY_DIR}/generated/task-300)
set(XR_PROGRAM_AOT_GENERATED_C
    ${XR_PROGRAM_AOT_FIXTURE_DIR}/xr_program_aot_generated.c)
add_custom_command(
    OUTPUT ${XR_PROGRAM_AOT_GENERATED_C}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${XR_PROGRAM_AOT_FIXTURE_DIR}
    COMMAND $<TARGET_FILE:test_xr_program_aot> ${XR_PROGRAM_AOT_GENERATED_C}
    DEPENDS test_xr_program_aot
    COMMENT "Generating canonical XrProgram portable C fixture"
    VERBATIM)
set_source_files_properties(${XR_PROGRAM_AOT_GENERATED_C} PROPERTIES GENERATED TRUE)
add_executable(test_xr_program_aot_native ${XR_PROGRAM_AOT_GENERATED_C})
xr_enable_pure_aot_symbol_map(test_xr_program_aot_native)
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC"
        OR (CMAKE_C_COMPILER_ID STREQUAL "Clang"
            AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    target_compile_options(test_xr_program_aot_native PRIVATE /W4 /WX)
else()
    target_compile_options(test_xr_program_aot_native PRIVATE
        -std=c11 -pedantic-errors -Wall -Wextra -Werror)
endif()
add_test(NAME test_xr_program_aot_native
    COMMAND ${XRAY_PYTHON}
        ${CMAKE_SOURCE_DIR}/scripts/check_xr_program_aot_native.py
        --executable $<TARGET_FILE:test_xr_program_aot_native>
        --expected-exit 42)
set_tests_properties(test_xr_program_aot_native PROPERTIES
    LABELS "unit;aot;canonical-program;generated-c;native;pure-aot;task-300")
add_test(NAME test_xr_program_aot_real_artifact
    COMMAND $<TARGET_FILE:test_xr_program_aot>
        --seal $<TARGET_FILE:test_xr_program_aot_native>)
set_tests_properties(test_xr_program_aot_real_artifact PROPERTIES
    LABELS "unit;aot;canonical-program;native-artifact;identity;task-300")

function(add_xr_program_aot_native_case case_name mode expected_exit)
    set(case_output_check)
    if(ARGC GREATER 3)
        list(APPEND case_output_check --expected-stdout-hex ${ARGV3})
    endif()
    set(case_source
        ${XR_PROGRAM_AOT_FIXTURE_DIR}/xr_program_aot_${case_name}.c)
    add_custom_command(
        OUTPUT ${case_source}
        COMMAND $<TARGET_FILE:test_xr_program_aot> ${case_source} ${mode}
        DEPENDS test_xr_program_aot
        COMMENT "Generating ${case_name} canonical XrProgram C fixture"
        VERBATIM)
    set_source_files_properties(${case_source} PROPERTIES GENERATED TRUE)
    add_executable(test_xr_program_aot_${case_name} ${case_source})
    target_link_libraries(test_xr_program_aot_${case_name} PRIVATE xray_native_provider_runtime)
    xr_enable_pure_aot_symbol_map(test_xr_program_aot_${case_name})
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC"
            OR (CMAKE_C_COMPILER_ID STREQUAL "Clang"
                AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
        target_compile_options(test_xr_program_aot_${case_name} PRIVATE /W4 /WX)
    else()
        target_compile_options(test_xr_program_aot_${case_name} PRIVATE
            -std=c11 -pedantic-errors -Wall -Wextra -Werror)
    endif()
    add_test(NAME test_xr_program_aot_${case_name}
        COMMAND ${XRAY_PYTHON}
            ${CMAKE_SOURCE_DIR}/scripts/check_xr_program_aot_native.py
            --executable $<TARGET_FILE:test_xr_program_aot_${case_name}>
            --expected-exit ${expected_exit} ${case_output_check})
    set_tests_properties(test_xr_program_aot_${case_name} PROPERTIES
        LABELS "unit;aot;canonical-program;generated-c;native;arithmetic;task-300")
endfunction()

foreach(loan_case RANGE 0 4)
    add_xr_program_aot_native_case(element_loan_${loan_case} element-loan-${loan_case} 0)
endforeach()
foreach(element_case RANGE 0 5)
    add_xr_program_aot_native_case(element_place_${element_case} element-place-${element_case} 0)
endforeach()
add_xr_program_aot_native_case(record_reference record-reference 0)
add_xr_program_aot_native_case(f64_comparisons f64-comparisons 1)
add_xr_program_aot_native_case(atomic_i64 atomic-i64 42)
add_xr_program_aot_native_case(atomic_bool atomic-bool 1)
add_xr_program_aot_native_case(atomic_i64_cas_1 atomic-i64-cas-1 42)
add_xr_program_aot_native_case(atomic_i64_cas_2 atomic-i64-cas-2 40)
add_xr_program_aot_native_case(atomic_i64_cas_3 atomic-i64-cas-3 40)
add_xr_program_aot_native_case(atomic_i64_cas_4 atomic-i64-cas-4 40)
add_xr_program_aot_native_case(atomic_bool_cas_1 atomic-bool-cas-1 1)
add_xr_program_aot_native_case(atomic_bool_cas_2 atomic-bool-cas-2 0)
add_xr_program_aot_native_case(atomic_bool_cas_3 atomic-bool-cas-3 0)
add_xr_program_aot_native_case(atomic_bool_cas_4 atomic-bool-cas-4 0)
add_xr_program_aot_native_case(atomic_i64_update_5 atomic-i64-cas-5 42)
add_xr_program_aot_native_case(atomic_i64_update_6 atomic-i64-cas-6 38)
add_xr_program_aot_native_case(atomic_i64_update_7 atomic-i64-cas-7 42)
add_xr_program_aot_native_case(atomic_i64_update_8 atomic-i64-cas-8 40)
add_xr_program_aot_native_case(atomic_i64_update_9 atomic-i64-cas-9 40)
add_xr_program_aot_native_case(atomic_i64_update_10 atomic-i64-cas-10 40)
add_xr_program_aot_native_case(atomic_bool_update_7 atomic-bool-cas-7 1)
add_xr_program_aot_native_case(atomic_bool_update_10 atomic-bool-cas-10 0)
add_xr_program_aot_native_case(atomic_i64_allocations atomic-i64-allocations 0)
add_xr_program_aot_native_case(atomic_bool_allocations atomic-bool-allocations 0)
add_xr_program_aot_native_case(array_alias array-alias 0)
add_xr_program_aot_native_case(array_strings array-values-0 2)
add_xr_program_aot_native_case(array_empty array-values-1 0)
add_xr_program_aot_native_case(array_integers array-values-2 2)
add_xr_program_aot_native_case(array_nested array-values-3 1)
foreach(scenario RANGE 0 4)
    add_xr_program_aot_native_case(array_allocations_${scenario} array-allocations-${scenario} 0)
endforeach()
add_xr_program_aot_native_case(integer_values integer-values 0)
add_xr_program_aot_native_case(integer_conversions integer-conversions 0)
add_xr_program_aot_native_case(string_slice string-slice 0)
add_xr_program_aot_native_case(array_default array-default 0)
add_xr_program_aot_native_case(array_append array-append 0)
add_xr_program_aot_native_case(array_append_allocations array-append-allocations 0)
add_xr_program_aot_native_case(array_append_managed_allocations array-append-managed-allocations 0)
foreach(byte_case RANGE 0 8)
    add_xr_program_aot_native_case(byte_compare_${byte_case} byte-compare-${byte_case} 0)
endforeach()
foreach(builder_case RANGE 0 3)
    add_xr_program_aot_native_case(string_builder_${builder_case} string-builder-${builder_case} 0)
endforeach()
add_xr_program_aot_native_case(array_default_allocations array-default-allocations 0)
add_xr_program_aot_native_case(string_slice_allocations string-slice-allocations 0)
add_xr_program_aot_native_case(integer_bitwise_values integer-bitwise 0)
add_xr_program_aot_native_case(integer_divmod integer-divmod 0)
add_xr_program_aot_native_case(integer_divmod_cleanup integer-divmod-cleanup 0)
foreach(outcome RANGE 0 7)
    add_xr_program_aot_native_case(coroutine_outcome_${outcome} coroutine-outcome-${outcome} 0)
    set_tests_properties(test_xr_program_aot_coroutine_outcome_${outcome} PROPERTIES
        LABELS "unit;aot;canonical-program;generated-c;native;coroutine;error;panic;ownership;task-293")
endforeach()
add_xr_program_aot_native_case(checked_overflow checked-overflow 211)
add_xr_program_aot_native_case(wrapping_overflow wrapping-overflow 0)
add_xr_program_aot_native_case(division_zero division-zero 212)
add_xr_program_aot_native_case(existential existential 42)
add_xr_program_aot_native_case(callable callable 42)
add_xr_program_aot_native_case(condition_assert condition-assert 0)
add_xr_program_aot_native_case(pointer_width pointer-width 32)
add_xr_program_aot_native_case(operating_system operating-system 4)
add_xr_program_aot_native_case(architecture architecture 5)
add_xr_program_aot_native_case(native_abi native-abi 9)
add_xr_program_aot_native_case(endianness endianness 1)
add_xr_program_aot_native_case(provider_call provider-call 42)
add_xr_program_aot_native_case(provider_pipe provider-pipe 1)
add_xr_program_aot_native_case(coroutine_yield coroutine-yield 42)
add_xr_program_aot_native_case(module_output module-output 0 310a320a330a340a)
add_xr_program_aot_native_case(module_suspend module-suspend 0 310a320a330a340a)
add_xr_program_aot_native_case(module_failure module-failure 214 310a320a)
add_xr_program_aot_native_case(owned_drop owned-drop 0 6f776e65642d64726f702d6e61746976653a20504153530a)

set(XR_PROGRAM_AOT_OUTPUT_SOURCE
    ${XR_PROGRAM_AOT_FIXTURE_DIR}/xr_program_aot_provider_output.c)
add_custom_command(
    OUTPUT ${XR_PROGRAM_AOT_OUTPUT_SOURCE}
    COMMAND $<TARGET_FILE:test_xr_program_aot>
        ${XR_PROGRAM_AOT_OUTPUT_SOURCE} provider-output
    DEPENDS test_xr_program_aot
    COMMENT "Generating provider-output canonical XrProgram C fixture"
    VERBATIM)
set_source_files_properties(${XR_PROGRAM_AOT_OUTPUT_SOURCE} PROPERTIES GENERATED TRUE)
add_executable(test_xr_program_aot_provider_output ${XR_PROGRAM_AOT_OUTPUT_SOURCE})
xr_enable_pure_aot_symbol_map(test_xr_program_aot_provider_output)
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC"
        OR (CMAKE_C_COMPILER_ID STREQUAL "Clang"
            AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    target_compile_options(test_xr_program_aot_provider_output PRIVATE /W4 /WX)
else()
    target_compile_options(test_xr_program_aot_provider_output PRIVATE
        -std=c11 -pedantic-errors -Wall -Wextra -Werror)
endif()
add_test(NAME test_xr_program_aot_provider_output
    COMMAND ${XRAY_PYTHON}
        ${CMAKE_SOURCE_DIR}/scripts/check_xr_program_aot_native.py
        --executable $<TARGET_FILE:test_xr_program_aot_provider_output>
        --expected-exit 0
        --expected-stdout-hex 34320a)
set_tests_properties(test_xr_program_aot_provider_output PROPERTIES
    LABELS "unit;aot;canonical-program;generated-c;native;provider;pure-aot;task-300")

set(XR_PROGRAM_AOT_TEXT_OUTPUT_SOURCE
    ${XR_PROGRAM_AOT_FIXTURE_DIR}/xr_program_aot_text_output.c)
add_custom_command(
    OUTPUT ${XR_PROGRAM_AOT_TEXT_OUTPUT_SOURCE}
    COMMAND $<TARGET_FILE:test_xr_program_aot>
        ${XR_PROGRAM_AOT_TEXT_OUTPUT_SOURCE} text-output
    DEPENDS test_xr_program_aot
    COMMENT "Generating typed text output canonical XrProgram C fixture"
    VERBATIM)
set_source_files_properties(${XR_PROGRAM_AOT_TEXT_OUTPUT_SOURCE} PROPERTIES GENERATED TRUE)
add_executable(test_xr_program_aot_text_output ${XR_PROGRAM_AOT_TEXT_OUTPUT_SOURCE})
xr_enable_pure_aot_symbol_map(test_xr_program_aot_text_output)
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC"
        OR (CMAKE_C_COMPILER_ID STREQUAL "Clang"
            AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    target_compile_options(test_xr_program_aot_text_output PRIVATE /W4 /WX)
else()
    target_compile_options(test_xr_program_aot_text_output PRIVATE
        -std=c11 -pedantic-errors -Wall -Wextra -Werror)
endif()
# Expected bytes: "-5 true abcd <U+1F600> true abcd\nabcd\n\n"
add_test(NAME test_xr_program_aot_text_output
    COMMAND ${XRAY_PYTHON}
        ${CMAKE_SOURCE_DIR}/scripts/check_xr_program_aot_native.py
        --executable $<TARGET_FILE:test_xr_program_aot_text_output>
        --expected-exit 0
        --expected-stdout-hex 2d352074727565206162636420f09f9880207472756520616263640a616263640a0a)
set_tests_properties(test_xr_program_aot_text_output PROPERTIES
    LABELS "unit;aot;canonical-program;generated-c;native;provider;string;pure-aot;task-309")

add_executable(xr_typed_provider_native_writer aot/xr_typed_provider_native_writer.c
    ${XR_TARGET_PROFILE_TEST_FIXTURE})
target_include_directories(xr_typed_provider_native_writer PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(xr_typed_provider_native_writer PRIVATE xr_program_test_core xray_program_aot_compiler xray_program_vm_runtime)
foreach(mode local module bytes empty bytes_module)
    set(source ${XR_PROGRAM_AOT_FIXTURE_DIR}/xr_typed_provider_${mode}_native.c)
    set(object xr_typed_provider_${mode}_native_object)
    set(target test_xr_typed_provider_${mode}_native)
    add_custom_command(OUTPUT ${source}
        COMMAND $<TARGET_FILE:xr_typed_provider_native_writer> ${source} ${mode}
        DEPENDS xr_typed_provider_native_writer VERBATIM)
    add_library(${object} OBJECT ${source})
    if(MSVC OR CMAKE_C_SIMULATE_ID STREQUAL "MSVC")
        target_compile_options(${object} PRIVATE /W4 /WX)
    else()
        target_compile_options(${object} PRIVATE -std=c11 -pedantic-errors -Wall -Wextra -Werror)
    endif()
    if(mode STREQUAL "bytes" OR mode STREQUAL "empty" OR mode STREQUAL "bytes_module")
        set(provider_test_source aot/test_xr_typed_provider_bytes_native.c)
    else()
        set(provider_test_source aot/test_xr_typed_provider_native.c)
    endif()
    add_executable(${target} ${provider_test_source}
        ${XR_TARGET_PROFILE_TEST_FIXTURE} $<TARGET_OBJECTS:${object}>)
    target_compile_definitions(${target} PRIVATE XR_TYPED_PROVIDER_MODULE=$<STREQUAL:${mode},module>)
    target_compile_definitions(${target} PRIVATE XR_TYPED_PROVIDER_EMPTY=$<STREQUAL:${mode},empty>)
    target_compile_definitions(${target} PRIVATE XR_TYPED_PROVIDER_PERSISTENT=$<STREQUAL:${mode},bytes_module>)
    target_include_directories(${target} PRIVATE ${XRAY_COMMON_INCLUDES})
    target_link_libraries(${target} PRIVATE xr_program_test_core xray_program_vm_runtime)
    add_test(NAME ${target} COMMAND $<TARGET_FILE:${target}>)
    set_tests_properties(${target} PROPERTIES LABELS "unit;aot;native;provider;ownership;generated-c")
endforeach()

foreach(mode local module bytes empty)
    set(target test_xr_typed_provider_${mode}_native_allocations)
    set(provider_allocation_object xr_typed_provider_${mode}_native_object)
    if(mode STREQUAL "bytes" OR mode STREQUAL "empty")
        set(provider_test_source aot/test_xr_typed_provider_bytes_native.c)
        set(provider_allocation_object xr_typed_provider_${mode}_allocation_object)
        add_library(${provider_allocation_object} OBJECT
            ${XR_PROGRAM_AOT_FIXTURE_DIR}/xr_typed_provider_${mode}_native.c)
        if(MSVC OR CMAKE_C_SIMULATE_ID STREQUAL "MSVC")
            target_compile_options(${provider_allocation_object} PRIVATE /W4 /WX
                "/FI${CMAKE_CURRENT_SOURCE_DIR}/aot/xr_generated_c_allocation_probe.h")
        else()
            target_compile_options(${provider_allocation_object} PRIVATE
                -std=c11 -pedantic-errors -Wall -Wextra -Werror
                -include "${CMAKE_CURRENT_SOURCE_DIR}/aot/xr_generated_c_allocation_probe.h")
        endif()
    else()
        set(provider_test_source aot/test_xr_typed_provider_native.c)
    endif()
    add_executable(${target} ${provider_test_source}
        ${XR_TARGET_PROFILE_TEST_FIXTURE}
        $<TARGET_OBJECTS:${provider_allocation_object}>
        $<TARGET_OBJECTS:xr_execution_allocation_probe>
        $<TARGET_OBJECTS:xr_native_execution_allocation_probe>)
    target_compile_definitions(${target} PRIVATE XR_NATIVE_RESOURCE_ALLOCATION_TEST=1
        XR_TYPED_PROVIDER_MODULE=$<STREQUAL:${mode},module>
        XR_TYPED_PROVIDER_EMPTY=$<STREQUAL:${mode},empty> XR_TYPED_PROVIDER_PERSISTENT=0)
    target_include_directories(${target} PRIVATE ${XRAY_COMMON_INCLUDES})
    target_link_libraries(${target} PRIVATE xr_program_test_core xray_program_vm_runtime)
    add_test(NAME ${target} COMMAND $<TARGET_FILE:${target}>)
    set_tests_properties(${target} PROPERTIES LABELS "unit;aot;native;provider;ownership;allocation-failure;generated-c")
endforeach()

set(XR_PROGRAM_AOT_COROUTINE_STEP_C
    ${XR_PROGRAM_AOT_FIXTURE_DIR}/xr_program_aot_coroutine_step.c)
add_custom_command(
    OUTPUT ${XR_PROGRAM_AOT_COROUTINE_STEP_C}
    COMMAND $<TARGET_FILE:test_xr_program_aot>
        ${XR_PROGRAM_AOT_COROUTINE_STEP_C} coroutine-yield-object
    DEPENDS test_xr_program_aot
    COMMENT "Generating coroutine-yield native step fixture"
    VERBATIM)
set_source_files_properties(${XR_PROGRAM_AOT_COROUTINE_STEP_C} PROPERTIES GENERATED TRUE)
add_library(test_xr_program_aot_coroutine_step_object OBJECT
    ${XR_PROGRAM_AOT_COROUTINE_STEP_C})
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC"
        OR (CMAKE_C_COMPILER_ID STREQUAL "Clang"
            AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    target_compile_options(test_xr_program_aot_coroutine_step_object PRIVATE /W4 /WX)
else()
    target_compile_options(test_xr_program_aot_coroutine_step_object PRIVATE
        -std=c11 -pedantic-errors -Wall -Wextra -Werror)
endif()
add_executable(test_xr_program_aot_coroutine_native_step
    aot/test_xr_program_aot_coroutine_native_step.c
    ${XR_TARGET_PROFILE_TEST_FIXTURE}
    $<TARGET_OBJECTS:test_xr_program_aot_coroutine_step_object>)
target_compile_definitions(test_xr_program_aot_coroutine_native_step
    PRIVATE XR_TEST_HAS_PROCESS_SHUTDOWN=1)
target_include_directories(test_xr_program_aot_coroutine_native_step PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_xr_program_aot_coroutine_native_step PRIVATE
    xr_program_test_core xray_program_vm_runtime xray_program_aot_compiler)
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC"
        OR (CMAKE_C_COMPILER_ID STREQUAL "Clang"
            AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    target_compile_options(test_xr_program_aot_coroutine_native_step PRIVATE /W4 /WX)
else()
    target_compile_options(test_xr_program_aot_coroutine_native_step PRIVATE
        -Wall -Wextra -Werror)
endif()
add_test(NAME test_xr_program_aot_coroutine_native_step
    COMMAND $<TARGET_FILE:test_xr_program_aot_coroutine_native_step>)
set_tests_properties(test_xr_program_aot_coroutine_native_step PROPERTIES
    LABELS "unit;aot;canonical-program;generated-c;native;generation;task-301")

add_library(xr_program_source_allocation_probe OBJECT
    ${CMAKE_SOURCE_DIR}/src/program/xr_program_source_build.c
    ${CMAKE_SOURCE_DIR}/src/program/xr_program_from_xi.c)
target_include_directories(xr_program_source_allocation_probe PRIVATE ${XRAY_COMMON_INCLUDES})
target_compile_definitions(xr_program_source_allocation_probe PRIVATE
    xr_program_source_build_default_budget=xr_source_probe_default_budget
    xr_program_source_build=xr_source_probe_build
    xr_program_source_product_free=xr_source_probe_product_free
    xr_program_source_build_status_name=xr_source_probe_status_name
    xr_program_write_from_xi=xr_source_probe_write_from_xi)
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC"
        OR (CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    target_compile_options(xr_program_source_allocation_probe PRIVATE
        /W4 /WX "/FI${CMAKE_CURRENT_SOURCE_DIR}/program/xr_program_source_allocation_probe.h")
else()
    target_compile_options(xr_program_source_allocation_probe PRIVATE
        -Wall -Wextra -Werror -include "${CMAKE_CURRENT_SOURCE_DIR}/program/xr_program_source_allocation_probe.h")
endif()
add_xray_bootstrap_unit_test(test_xr_program_source_allocations
    program/test_xr_program_source_allocations.c plan/target_profile_test_fixture.c
    $<TARGET_OBJECTS:xr_program_source_allocation_probe>)
target_include_directories(test_xr_program_source_allocations PRIVATE ${XR_PROGRAM_SOURCE_FIXTURE_DIR})
target_compile_definitions(test_xr_program_source_allocations PRIVATE
    XR_OUTPUT_TRAP_NATIVE_HARNESS="${CMAKE_CURRENT_SOURCE_DIR}/aot/xr_program_output_trap_native_harness.c"
    XR_MODULE_REPORT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/module-report")
set_tests_properties(test_xr_program_source_allocations PROPERTIES
    LABELS "unit;canonical-program;source-owner;allocation-failure;ownership")

add_library(xr_program_vm_allocation_probe OBJECT ${CMAKE_SOURCE_DIR}/src/vm/xr_program_vm.c)
target_include_directories(xr_program_vm_allocation_probe PRIVATE ${XRAY_COMMON_INCLUDES})
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC"
        OR (CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    target_compile_options(xr_program_vm_allocation_probe PRIVATE
        /W4 /WX "/FI${CMAKE_CURRENT_SOURCE_DIR}/program/xr_program_allocation_probe.h")
else()
    target_compile_options(xr_program_vm_allocation_probe PRIVATE
        -Wall -Wextra -Werror -include "${CMAKE_CURRENT_SOURCE_DIR}/program/xr_program_allocation_probe.h")
endif()
add_executable(test_xr_program_vm_allocations
    vm/test_xr_program_vm_allocations.c ${XR_TARGET_PROFILE_TEST_FIXTURE}
    $<TARGET_OBJECTS:xr_program_vm_allocation_probe>)
target_include_directories(test_xr_program_vm_allocations PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(test_xr_program_vm_allocations PRIVATE xr_program_test_core xray_program_vm_runtime)
add_test(NAME test_xr_program_vm_allocations COMMAND $<TARGET_FILE:test_xr_program_vm_allocations>)
set_tests_properties(test_xr_program_vm_allocations PROPERTIES
    LABELS "unit;vm;canonical-program;allocation-failure;generation;task-310")

add_library(xr_native_execution_allocation_probe OBJECT
    ${CMAKE_SOURCE_DIR}/src/execution/xr_native_execution.c)
target_include_directories(xr_native_execution_allocation_probe PRIVATE ${XRAY_COMMON_INCLUDES})
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC"
        OR (CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    target_compile_options(xr_native_execution_allocation_probe PRIVATE
        /W4 /WX "/FI${CMAKE_CURRENT_SOURCE_DIR}/program/xr_program_allocation_probe.h")
else()
    target_compile_options(xr_native_execution_allocation_probe PRIVATE
        -Wall -Wextra -Werror -include "${CMAKE_CURRENT_SOURCE_DIR}/program/xr_program_allocation_probe.h")
endif()
add_executable(test_xr_native_execution_allocations
    aot/test_xr_native_execution_allocations.c ${XR_TARGET_PROFILE_TEST_FIXTURE}
    $<TARGET_OBJECTS:xr_native_execution_allocation_probe>
    $<TARGET_OBJECTS:test_xr_program_aot_coroutine_step_object>)
target_include_directories(test_xr_native_execution_allocations PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(test_xr_native_execution_allocations PRIVATE
    xr_program_test_core xray_program_vm_runtime xray_program_aot_compiler)
add_test(NAME test_xr_native_execution_allocations COMMAND $<TARGET_FILE:test_xr_native_execution_allocations>)
set_tests_properties(test_xr_native_execution_allocations PROPERTIES
    LABELS "unit;aot;canonical-program;native;allocation-failure;generation;task-310")

foreach(module_case IN ITEMS output failure suspend)
    set(module_target test_xr_program_aot_module_instance_${module_case})
    set(module_source ${XR_PROGRAM_AOT_FIXTURE_DIR}/module_instance_${module_case}.c)
    add_custom_command(
        OUTPUT ${module_source}
        COMMAND $<TARGET_FILE:test_xr_program_aot> ${module_source} module-${module_case}-object
        DEPENDS test_xr_program_aot
        COMMENT "Generating native module instance ${module_case} fixture"
        VERBATIM)
    set_source_files_properties(${module_source} PROPERTIES GENERATED TRUE)
    add_executable(${module_target}
        aot/test_xr_program_aot_module_instance.c
        ${XR_TARGET_PROFILE_TEST_FIXTURE}
        ${module_source})
    if(WIN32)
        target_sources(${module_target} PRIVATE ${CMAKE_SOURCE_DIR}/src/os/win/thread_win.c)
    else()
        target_sources(${module_target} PRIVATE ${CMAKE_SOURCE_DIR}/src/os/unix/thread_unix.c)
    endif()
    target_compile_definitions(${module_target} PRIVATE
        XR_TEST_HAS_PROCESS_SHUTDOWN=1
        XR_NATIVE_MODULE_FAILURE=$<STREQUAL:${module_case},failure>
        XR_NATIVE_MODULE_SUSPEND=$<STREQUAL:${module_case},suspend>)
    target_include_directories(${module_target} PRIVATE ${XRAY_COMMON_INCLUDES} ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${module_target} PRIVATE
        xr_program_test_core xray_program_vm_runtime xray_program_aot_compiler)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC"
            OR (CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
        target_compile_options(${module_target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${module_target} PRIVATE -std=c11 -pedantic-errors -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${module_target} COMMAND $<TARGET_FILE:${module_target}>)
    set_tests_properties(${module_target} PROPERTIES
        LABELS "unit;aot;canonical-program;generated-c;native;generation;module;task-310")
endforeach()

foreach(module_scenario RANGE 0 13)
    set(module_target test_xr_program_aot_module_state_${module_scenario})
    set(module_source ${XR_PROGRAM_AOT_FIXTURE_DIR}/module_state_${module_scenario}.c)
    add_custom_command(
        OUTPUT ${module_source}
        COMMAND $<TARGET_FILE:test_xr_program_aot> ${module_source} module-state-${module_scenario}
        DEPENDS test_xr_program_aot
        COMMENT "Generating native typed module state ${module_scenario} fixture"
        VERBATIM)
    set_source_files_properties(${module_source} PROPERTIES GENERATED TRUE HEADER_FILE_ONLY TRUE)
    add_executable(${module_target}
        aot/test_xr_program_aot_module_state.c ${XR_TARGET_PROFILE_TEST_FIXTURE} ${module_source})
    target_compile_definitions(${module_target} PRIVATE
        XR_TEST_HAS_PROCESS_SHUTDOWN=1 XR_MODULE_STATE_SCENARIO=${module_scenario}
        XR_MODULE_STATE_GENERATED_C="module_state_${module_scenario}.c")
    target_include_directories(${module_target} PRIVATE
        ${XRAY_COMMON_INCLUDES} ${CMAKE_CURRENT_SOURCE_DIR} ${XR_PROGRAM_AOT_FIXTURE_DIR})
    target_link_libraries(${module_target} PRIVATE
        xr_program_test_core xray_program_vm_runtime xray_program_aot_compiler)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC"
            OR (CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
        target_compile_options(${module_target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${module_target} PRIVATE -std=c11 -pedantic-errors -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${module_target} COMMAND $<TARGET_FILE:${module_target}>)
    set_tests_properties(${module_target} PROPERTIES
        LABELS "unit;aot;canonical-program;generated-c;native;module;ownership;task-310")
endforeach()

set(XR_PROGRAM_AOT_INVOKE_C
    ${XR_PROGRAM_AOT_FIXTURE_DIR}/xr_program_aot_sealed_invoke.c)
add_custom_command(
    OUTPUT ${XR_PROGRAM_AOT_INVOKE_C}
    COMMAND $<TARGET_FILE:test_xr_program_aot>
        ${XR_PROGRAM_AOT_INVOKE_C} sealed-invoke-object
    DEPENDS test_xr_program_aot
    COMMENT "Generating sealed-invoke canonical XrProgram C object fixture"
    VERBATIM)
set_source_files_properties(${XR_PROGRAM_AOT_INVOKE_C} PROPERTIES GENERATED TRUE)
add_library(test_xr_program_aot_sealed_invoke_object OBJECT ${XR_PROGRAM_AOT_INVOKE_C})
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC"
        OR (CMAKE_C_COMPILER_ID STREQUAL "Clang"
            AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    target_compile_options(test_xr_program_aot_sealed_invoke_object PRIVATE /W4 /WX)
else()
    target_compile_options(test_xr_program_aot_sealed_invoke_object PRIVATE
        -std=c11 -pedantic-errors -Wall -Wextra -Werror)
endif()

add_test(NAME test_xr_program_aot_providers
    COMMAND ${XRAY_PYTHON}
        ${CMAKE_SOURCE_DIR}/scripts/check_xr_program_aot_providers.py
        --source ${XR_PROGRAM_AOT_GENERATED_C}
        --output-dir ${XR_PROGRAM_AOT_FIXTURE_DIR}/providers
        --expected-exit 42)
set_tests_properties(test_xr_program_aot_providers PROPERTIES
    LABELS "aot;canonical-program;generated-c;providers;portable-c11;task-300"
    RESOURCE_LOCK xray_native_toolchain
    TIMEOUT 120)
add_xray_unit_test(test_target_plan plan/test_target_plan.c
    ${XR_TARGET_PROFILE_TEST_FIXTURE})
add_test(NAME test_target_class_argument_authority
    COMMAND $<TARGET_FILE:test_target_plan> direct-local-class-argument-authority)
set_tests_properties(test_target_class_argument_authority PROPERTIES
    LABELS "target-machine;aot;ownership;class;const")
add_test(NAME test_target_program_reachability
    COMMAND $<TARGET_FILE:test_target_plan> program-reachability)
set_tests_properties(test_target_program_reachability PROPERTIES
    LABELS "target-machine;aot;typed-plan;reachability")
add_test(NAME test_target_native_direct_ref_authority
    COMMAND $<TARGET_FILE:test_target_plan> native-direct-ref-authority)
set_tests_properties(test_target_native_direct_ref_authority PROPERTIES
    LABELS "target-machine;aot;typed-plan;stdlib")
add_test(NAME test_target_assertion_capability_authority
    COMMAND $<TARGET_FILE:test_target_plan> assertion-capability-authority)
add_test(NAME test_target_array_intrinsic_authority
    COMMAND $<TARGET_FILE:test_target_plan> array-intrinsic-authority)
add_test(NAME test_target_map_entry_iterator_authority
    COMMAND $<TARGET_FILE:test_target_plan> map-entry-iterator-authority)
add_test(NAME test_target_iterator_rune_nth_argument_authority
    COMMAND $<TARGET_FILE:test_target_plan> iterator-rune-nth-argument-authority)
add_test(NAME test_target_source_class_array_fill_authority
    COMMAND $<TARGET_FILE:test_target_plan> source-class-array-fill-authority)
add_test(NAME test_target_builtin_runtime_method_authority
    COMMAND $<TARGET_FILE:test_target_plan> builtin-runtime-method-authority)
set_tests_properties(test_target_builtin_runtime_method_authority PROPERTIES
    LABELS "target-machine;aot;typed-plan;runtime-method")
add_xray_unit_test(test_xtp_format plan/test_xtp_format.c
    ${XR_TARGET_PROFILE_TEST_FIXTURE})
add_xray_unit_test(test_xtp_resource_stress
    plan/test_xtp_resource_stress.c)
if(WIN32)
    target_link_libraries(test_xtp_resource_stress PRIVATE psapi)
endif()
set_tests_properties(test_xtp_resource_stress PROPERTIES
    LABELS "target-machine;vm;typed-plan;loader;stress"
    PROCESSORS 1
    TIMEOUT 90)
add_executable(test_runtime_target_plan_load_archive
    runtime/test_runtime_target_plan_load_archive.c)
set(XR_RUNTIME_SCALAR_ARTIFACT_DIR
    ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(XR_RUNTIME_SCALAR_ARTIFACT_HEADER
    ${XR_RUNTIME_SCALAR_ARTIFACT_DIR}/runtime_scalar_artifacts.h)
add_custom_command(
    OUTPUT ${XR_RUNTIME_SCALAR_ARTIFACT_HEADER}
    COMMAND ${CMAKE_COMMAND} -E make_directory
            ${XR_RUNTIME_SCALAR_ARTIFACT_DIR}
    COMMAND $<TARGET_FILE:test_xtp_format>
            --write-runtime-header ${XR_RUNTIME_SCALAR_ARTIFACT_HEADER}
    DEPENDS test_xtp_format
    COMMENT "Generating native exact XSM/XTP scalar runtime fixture"
    VERBATIM)
add_custom_target(runtime_scalar_artifact_fixture
    DEPENDS ${XR_RUNTIME_SCALAR_ARTIFACT_HEADER})
# The bounded program capability needs an arbitrarily ordered XSM module set
# plus one exact XTP, which no single-module fixture can stand in for. The
# writer is a CLI mode of the closure test rather than one of its cases, so it
# produces the artifacts without running them.
set(XR_RUNTIME_PROGRAM_ARTIFACT_DIR
    ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(XR_RUNTIME_PROGRAM_ARTIFACTS
    ${XR_RUNTIME_PROGRAM_ARTIFACT_DIR}/runtime_program_0.xsm
    ${XR_RUNTIME_PROGRAM_ARTIFACT_DIR}/runtime_program_1.xsm
    ${XR_RUNTIME_PROGRAM_ARTIFACT_DIR}/runtime_program.xtp)
add_custom_command(
    OUTPUT ${XR_RUNTIME_PROGRAM_ARTIFACTS}
    COMMAND ${CMAKE_COMMAND} -E make_directory
            ${XR_RUNTIME_PROGRAM_ARTIFACT_DIR}
    COMMAND $<TARGET_FILE:test_xa_program_semantic_closure>
            --write-program-runtime-artifacts ${XR_RUNTIME_PROGRAM_ARTIFACTS}
    DEPENDS test_xa_program_semantic_closure
    COMMENT "Generating exact XSM module-set/XTP program runtime fixture"
    VERBATIM)
add_custom_target(runtime_program_artifact_fixture
    DEPENDS ${XR_RUNTIME_PROGRAM_ARTIFACTS})
add_dependencies(test_runtime_target_plan_load_archive
    runtime_scalar_artifact_fixture)
target_include_directories(test_runtime_target_plan_load_archive PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${XR_RUNTIME_SCALAR_ARTIFACT_DIR})
target_link_libraries(test_runtime_target_plan_load_archive PRIVATE
    xray_vm)
add_test(NAME test_runtime_target_plan_load_archive
    COMMAND $<TARGET_FILE:test_runtime_target_plan_load_archive>)
add_xray_unit_test(test_runtime_generation
    runtime/test_runtime_generation.c
    runtime/test_runtime_generation_concurrency.c
    runtime/test_runtime_generation_audit.c)
add_xray_unit_test(test_dynamic_entry_runtime
    runtime/test_dynamic_entry_runtime.c)
add_executable(test_runtime_generation_archive
    runtime/test_runtime_generation_archive.c)
target_include_directories(test_runtime_generation_archive PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_runtime_generation_archive PRIVATE
    xray_vm)
add_test(NAME test_runtime_generation_archive
    COMMAND $<TARGET_FILE:test_runtime_generation_archive>)
add_executable(test_runtime_api_archive
    runtime/test_runtime_api_archive.c)
add_dependencies(test_runtime_api_archive runtime_scalar_artifact_fixture)
target_include_directories(test_runtime_api_archive PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${XR_RUNTIME_SCALAR_ARTIFACT_DIR})
target_link_libraries(test_runtime_api_archive PRIVATE
    xray_vm)
add_test(NAME test_runtime_api_archive
    COMMAND $<TARGET_FILE:test_runtime_api_archive>)
add_executable(test_runtime_program_archive
    runtime/test_runtime_program_archive.c)
add_dependencies(test_runtime_program_archive
    runtime_program_artifact_fixture)
target_include_directories(test_runtime_program_archive PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(test_runtime_program_archive PRIVATE
    XR_RUNTIME_PROGRAM_ARTIFACT_DIR="${XR_RUNTIME_PROGRAM_ARTIFACT_DIR}")
target_link_libraries(test_runtime_program_archive PRIVATE
    xray_vm)
add_test(NAME test_runtime_program_archive
    COMMAND $<TARGET_FILE:test_runtime_program_archive>)
set_tests_properties(test_runtime_program_archive PROPERTIES
    LABELS "runtime;target-machine;typed-plan;vm;concurrency"
    PROCESSORS 1
    TIMEOUT 90)
add_executable(test_entry_cell_runtime_archive
    runtime/test_entry_cell_runtime_archive.c)
add_dependencies(test_entry_cell_runtime_archive
    runtime_scalar_artifact_fixture)
target_include_directories(test_entry_cell_runtime_archive PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${XR_RUNTIME_SCALAR_ARTIFACT_DIR})
target_link_libraries(test_entry_cell_runtime_archive PRIVATE
    xray_vm)
add_test(NAME test_entry_cell_runtime_archive
    COMMAND $<TARGET_FILE:test_entry_cell_runtime_archive>)
add_xray_unit_test(test_typed_frame vm/test_typed_frame.c
    ${XR_TARGET_PROFILE_TEST_FIXTURE})
add_test(NAME test_typed_lifecycle
    COMMAND $<TARGET_FILE:test_typed_frame> vm-lifecycle)
set_tests_properties(test_typed_lifecycle PROPERTIES
    LABELS "runtime;target-machine;typed-plan;vm;ownership")
add_xray_unit_test(test_typed_opaque_boundary
    vm/test_typed_opaque_boundary.c)
set_tests_properties(test_typed_opaque_boundary PROPERTIES
    LABELS "runtime;target-machine;typed-plan;vm;boundary")
add_xray_unit_test(test_typed_lifecycle_audit
    runtime/test_typed_lifecycle_audit.c
    ${XR_TARGET_PROFILE_TEST_FIXTURE}
    ${CMAKE_SOURCE_DIR}/src/runtime/ownership/xr_ownership_audit.c
    ${CMAKE_SOURCE_DIR}/src/vm/audit/xr_typed_lifecycle_audit.c)
set_tests_properties(test_typed_lifecycle_audit PROPERTIES
    LABELS "runtime;semantic-plan;target-machine;typed-plan;vm;ownership")
add_xray_unit_test(test_typed_dispatch vm/test_typed_dispatch.c
    ${XR_TARGET_PROFILE_TEST_FIXTURE})
add_xray_unit_test(test_vm_decoded_cache vm/test_vm_decoded_cache.c
    ${XR_TARGET_PROFILE_TEST_FIXTURE})
if(WIN32 AND CMAKE_BUILD_TYPE STREQUAL "Release")
    add_executable(typed_target_vm_benchmark
        ${CMAKE_SOURCE_DIR}/tests/benchmarks/target-machine/typed_target_vm/benchmark.c
        ${XR_TARGET_PROFILE_TEST_FIXTURE})
    target_compile_definitions(typed_target_vm_benchmark PRIVATE
        XR_TEST_HAS_PROCESS_SHUTDOWN=1)
    target_include_directories(typed_target_vm_benchmark PRIVATE
        ${XRAY_COMMON_INCLUDES}
        ${CMAKE_BINARY_DIR}/generated
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(typed_target_vm_benchmark PRIVATE xray_core)
    add_test(NAME typed_target_vm_performance_gate
        COMMAND ${XRAY_PYTHON}
            ${CMAKE_SOURCE_DIR}/tests/benchmarks/target-machine/typed_target_vm/run.py
            --benchmark $<TARGET_FILE:typed_target_vm_benchmark>
            --policy ${CMAKE_SOURCE_DIR}/contracts/target-machine/baseline-manifest.json)
    set_tests_properties(typed_target_vm_performance_gate PROPERTIES
        LABELS "performance;target-machine;vm;typed-plan"
        PROCESSORS 1
        TIMEOUT 30)
endif()
add_test(NAME typed_target_vm_performance_runner_self_test
    COMMAND ${XRAY_PYTHON}
        ${CMAKE_SOURCE_DIR}/tests/benchmarks/target-machine/typed_target_vm/run.py
        --self-test)
set_tests_properties(typed_target_vm_performance_runner_self_test PROPERTIES
    LABELS "performance;target-machine;self-test"
    PROCESSORS 1
    TIMEOUT 30)
add_test(NAME typed_target_vm_call_staging_contract
    COMMAND ${XRAY_PYTHON}
        ${CMAKE_SOURCE_DIR}/scripts/check_typed_call_staging.py
        --root ${CMAKE_SOURCE_DIR})
set_tests_properties(typed_target_vm_call_staging_contract PROPERTIES
    LABELS "performance;target-machine;typed-plan;vm")
add_test(NAME coroutine_lifecycle_projection_contract
    COMMAND ${XRAY_PYTHON}
        ${CMAKE_SOURCE_DIR}/scripts/check_coroutine_lifecycle_projection.py
        --root ${CMAKE_SOURCE_DIR})
set_tests_properties(coroutine_lifecycle_projection_contract PROPERTIES
    LABELS "performance;semantic-plan;target-machine;typed-plan;vm"
    PROCESSORS 1
    TIMEOUT 30)
add_test(NAME coroutine_lifecycle_projection_self_test
    COMMAND ${XRAY_PYTHON}
        ${CMAKE_SOURCE_DIR}/scripts/check_coroutine_lifecycle_projection.py
        --self-test)
set_tests_properties(coroutine_lifecycle_projection_self_test PROPERTIES
    LABELS "semantic-plan;target-machine;typed-plan;vm;self-test"
    PROCESSORS 1
    TIMEOUT 30)
add_executable(test_target_machine_comparison_fixture
    ../target_machine/comparison/typed_fixture.c
    ${XR_TARGET_PROFILE_TEST_FIXTURE})
target_include_directories(test_target_machine_comparison_fixture PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_target_machine_comparison_fixture PRIVATE xray_core)
add_dependencies(test_target_machine_comparison_fixture xray)
add_test(NAME target_machine_scalar_comparison
    COMMAND ${XRAY_PYTHON}
        ${CMAKE_SOURCE_DIR}/tests/target_machine/comparison/run_comparison.py
        --xray $<TARGET_FILE:xray>
        --typed-fixture $<TARGET_FILE:test_target_machine_comparison_fixture>
        --manifest ${CMAKE_SOURCE_DIR}/tests/target_machine/comparison/cases.json
        --report ${CMAKE_BINARY_DIR}/target-machine-scalar-comparison-report.json)
set_tests_properties(target_machine_scalar_comparison PROPERTIES
    LABELS "target-machine;comparison;vm;aot"
    TIMEOUT 300)
add_executable(test_typed_frame_runtime_archive
    runtime/test_typed_frame_runtime_archive.c)
target_include_directories(test_typed_frame_runtime_archive PRIVATE
    ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_typed_frame_runtime_archive PRIVATE
    xray_vm)
add_test(NAME test_typed_frame_runtime_archive
    COMMAND $<TARGET_FILE:test_typed_frame_runtime_archive>)
add_executable(test_vm_decoded_cache_runtime_archive
    runtime/test_vm_decoded_cache_runtime_archive.c)
target_include_directories(test_vm_decoded_cache_runtime_archive PRIVATE
    ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_vm_decoded_cache_runtime_archive PRIVATE
    xray_vm)
add_test(NAME test_vm_decoded_cache_runtime_archive
    COMMAND $<TARGET_FILE:test_vm_decoded_cache_runtime_archive>)
add_executable(test_xtp_fuzz_entry ../fuzz/fuzz_xtp_decode.c
    ${XR_TARGET_PROFILE_TEST_FIXTURE})
target_compile_definitions(test_xtp_fuzz_entry PRIVATE FUZZ_STANDALONE)
target_include_directories(test_xtp_fuzz_entry PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(test_xtp_fuzz_entry PRIVATE xray_core)
add_dependencies(test_xtp_fuzz_entry xray test_xtp_resource_stress)
add_test(NAME test_xtp_fuzz_entry COMMAND $<TARGET_FILE:test_xtp_fuzz_entry>)
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
add_test(
    NAME test_xtp_fuzz_evidence
    COMMAND ${XRAY_PYTHON}
        ${CMAKE_SOURCE_DIR}/tests/fuzz/run_xtp_fuzz_evidence.py
        --runtime $<TARGET_FILE:xray>
        --fuzzer $<TARGET_FILE:test_xtp_fuzz_entry>
        --resource-stress $<TARGET_FILE:test_xtp_resource_stress>
        --corpus ${CMAKE_SOURCE_DIR}/tests/fuzz/corpus/xtp
        --expected-commit ${XRAY_GIT_COMMIT}
        --sanitizer ${XR_XTP_FUZZ_EVIDENCE_SANITIZER}
)
set_tests_properties(test_xtp_fuzz_evidence PROPERTIES
    LABELS "target-machine;vm;typed-plan;loader;fuzz;stress"
    PROCESSORS 1
    TIMEOUT 90)
add_xray_unit_test(test_xr_aot_scalar_plan aot/test_xr_aot_scalar_plan.c
    ${XR_TARGET_PROFILE_TEST_FIXTURE})
add_test(NAME test_xr_aot_iterator_rune_nth_emission
    COMMAND $<TARGET_FILE:test_xr_aot_scalar_plan> iterator-rune-nth-emission)
add_test(NAME test_xr_aot_string_byte_slice_view_emission
    COMMAND $<TARGET_FILE:test_xr_aot_scalar_plan> string-byte-slice-view-emission)
add_xray_unit_test(test_xaot_scalar_target_cutover aot/test_xaot_scalar_target_cutover.c
    ${XR_TARGET_PROFILE_TEST_FIXTURE})
add_library(xaot_callable_allocation_probe OBJECT ${CMAKE_SOURCE_DIR}/src/aot/xaot_callable.c)
target_include_directories(xaot_callable_allocation_probe PRIVATE ${XRAY_COMMON_INCLUDES})
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC"
        OR (CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    target_compile_options(xaot_callable_allocation_probe PRIVATE
        /W4 /WX "/FI${CMAKE_CURRENT_SOURCE_DIR}/program/xr_program_allocation_probe.h")
else()
    target_compile_options(xaot_callable_allocation_probe PRIVATE
        -Wall -Wextra -Werror -include "${CMAKE_CURRENT_SOURCE_DIR}/program/xr_program_allocation_probe.h")
endif()
add_xray_unit_test(test_xaot_callable_allocations aot/test_xaot_callable_allocations.c
    $<TARGET_OBJECTS:xaot_callable_allocation_probe>)
set_tests_properties(test_xaot_callable_allocations PROPERTIES
    LABELS "unit;aot;allocation-failure;ownership")
add_xray_unit_test(test_xr_aot_refinement aot/test_xr_aot_refinement.c
    ${XR_TARGET_PROFILE_TEST_FIXTURE})
set_tests_properties(test_xr_aot_refinement PROPERTIES
    LABELS "aot;refinement;backend;task-279"
)
add_test(NAME test_xr_aot_refinement_unreachable_native_callee
    COMMAND $<TARGET_FILE:test_xr_aot_refinement> unreachable-native-callee)
set_tests_properties(test_xr_aot_refinement_unreachable_native_callee PROPERTIES
    LABELS "aot;refinement;target-plan;mutation;task-293"
)

# Keep the dumb-emitter contract self-contained even though the behavioral
# test later includes compiler-only fixtures. This compile probe has only the
# emission directory on its include path and fails if a forbidden layer leaks
# through the public emission headers.
include(CheckCSourceCompiles)
set(_XR_C_EMISSION_SAVED_REQUIRED_INCLUDES "${CMAKE_REQUIRED_INCLUDES}")
set(CMAKE_REQUIRED_INCLUDES "${PROJECT_SOURCE_DIR}/src/aot/emit_c")
unset(XR_C_EMISSION_HEADERS_ARE_ISOLATED CACHE)
check_c_source_compiles([=[
#include "xr_c_emission_schema.h"
#include "xr_c_emission_plan.h"
#if defined(XR_SEMANTIC_PLAN_H) || defined(XI_H) || defined(XAOT_REP_H) || defined(XTYPE_H)
#error "C emission headers exposed a forbidden compiler layer"
#endif
_Static_assert(XR_C_VALUE_REP_COUNT == 19, "value spelling schema changed");
int main(void) {
    XrCValueEmissionView view = {0};
    return (int) view.semantic_value;
}
]=] XR_C_EMISSION_HEADERS_ARE_ISOLATED)
set(CMAKE_REQUIRED_INCLUDES "${_XR_C_EMISSION_SAVED_REQUIRED_INCLUDES}")
unset(_XR_C_EMISSION_SAVED_REQUIRED_INCLUDES)
if(NOT XR_C_EMISSION_HEADERS_ARE_ISOLATED)
    message(FATAL_ERROR
        "C emission headers must compile without SemanticPlan, IR, Xaot, or xtype")
endif()
add_xray_unit_test(test_cache_key incremental/test_cache_key.c)
add_xray_unit_test(test_cache_store incremental/test_cache_store.c)
add_xray_unit_test(test_cache_artifact_verify
                   incremental/test_cache_artifact_verify.c
                   ${XR_TARGET_PROFILE_TEST_FIXTURE})
add_xray_unit_test(test_program_target_plan_build
                   incremental/test_program_target_plan_build.c
                   incremental/program_plan_cache_fixture.c
                   ${XR_TARGET_PROFILE_TEST_FIXTURE})
add_xray_unit_test(test_program_plan_cache_qualification
                   incremental/test_program_plan_cache_qualification.c
                   incremental/program_plan_cache_fixture.c
                   ${XR_TARGET_PROFILE_TEST_FIXTURE})
set_tests_properties(test_program_plan_cache_qualification PROPERTIES
    LABELS "cache;target-plan;concurrency;mutation")
add_xray_unit_test(test_dependency_graph incremental/test_dependency_graph.c)
add_xray_unit_test(test_module_summary_build incremental/test_module_summary_build.c)
add_xray_unit_test(test_compiler_session_generation
                   toolchain/test_compiler_session_generation.c)
add_xray_unit_test(test_fs_atomic os/test_fs_atomic.c)
add_xray_unit_test(test_xi_tbaa ir/test_xi_tbaa.c)
add_xray_unit_test(test_xi_range ir/test_xi_range.c)
add_xray_unit_test(test_xi_block_simplify ir/test_xi_block_simplify.c)
add_xray_unit_test(test_xi_jump_thread ir/test_xi_jump_thread.c)
add_xray_unit_test(test_xi_loop_iv ir/test_xi_loop_iv.c)
add_xray_unit_test(test_xi_loop_rotate ir/test_xi_loop_rotate.c)
add_xray_unit_test(test_xi_loop_unroll ir/test_xi_loop_unroll.c)
add_xray_unit_test(test_xi_loop_split ir/test_xi_loop_split.c)
add_xray_unit_test(test_xi_loop_inv_branch ir/test_xi_loop_inv_branch.c)
add_xray_unit_test(test_xi_vec_scalar ir/test_xi_vec_scalar.c)
add_xray_unit_test(test_xi_comptime ir/test_xi_comptime.c)
add_xray_unit_test(test_xaot_driver
    aot/test_xaot_driver.c
    ${CMAKE_SOURCE_DIR}/src/app/toolchain/xtc_model.c
    ${CMAKE_SOURCE_DIR}/src/app/toolchain/xtc_target_profile.c
)
set_tests_properties(test_xaot_driver PROPERTIES COST 130)
add_test(NAME test_xaot_driver_program_graph_target_plan_cache
    COMMAND ${CMAKE_COMMAND} -E env
            XRAY_TEST_FILTER=program_graph_target_plan_cache
            $<TARGET_FILE:test_xaot_driver>)
set_tests_properties(test_xaot_driver_program_graph_target_plan_cache PROPERTIES
    LABELS "aot;program-graph;cache;target-plan;mutation"
    TIMEOUT 120)
add_xray_unit_test(test_xaot_transfer_plan aot/test_xaot_transfer_plan.c)
add_xray_unit_test(test_cgen_verify_output aot/test_cgen_verify_output.c)
set_tests_properties(test_cgen_verify_output PROPERTIES LABELS "aot;cgen;task-218")
add_xray_unit_test(test_c_emission_typed_rules aot/test_c_emission_typed_rules.c)
set_tests_properties(test_c_emission_typed_rules PROPERTIES
    LABELS "aot;cgen;typed-rules;mutation")
add_xray_unit_test(test_xi_backend_plan_contract aot/test_xi_backend_plan_contract.c)
add_xray_unit_test(test_xrt_array aot/test_xrt_array.c)
add_executable(test_xrt_udp_retry aot/test_xrt_udp_retry.c)
target_include_directories(test_xrt_udp_retry PRIVATE ${XRAY_COMMON_INCLUDES} ${CMAKE_SOURCE_DIR}/include)
if(WIN32)
    target_link_libraries(test_xrt_udp_retry PRIVATE ws2_32)
endif()
add_test(NAME test_xrt_udp_retry COMMAND test_xrt_udp_retry)
set_tests_properties(test_xrt_udp_retry PROPERTIES TIMEOUT 15 LABELS "aot;runtime;network")

add_xray_unit_test(test_xrt_execution_arena aot/test_xrt_execution_arena.c)
set_tests_properties(test_xrt_execution_arena PROPERTIES LABELS "aot;rc;task-252")
add_xray_unit_test(test_xrt_boolmap aot/test_xrt_boolmap.c)
add_xray_unit_test(test_xrt_dense_enum aot/test_xrt_dense_enum.c)
add_xray_unit_test(test_xrt_json_decode aot/test_xrt_json_decode.c)
add_xray_unit_test(test_xrt_static_map_set aot/test_xrt_static_map_set.c)
add_xray_unit_test(test_xrt_exception_tls aot/test_xrt_exception_tls.c)
add_xray_unit_test(test_type_names_parity aot/test_type_names_parity.c)
set_tests_properties(test_type_names_parity PROPERTIES LABELS "aot;vm;shared-core")
add_xray_unit_test(test_xrt_method_truthy aot/test_xrt_method_truthy.c)
add_executable(test_xrt_assertion aot/test_xrt_assertion.c)
target_compile_definitions(test_xrt_assertion PRIVATE XR_TEST_HAS_PROCESS_SHUTDOWN=1)
target_include_directories(test_xrt_assertion PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include
)
# This case compiles the header-only runtime rather than linking xray_core, so
# it does not inherit the math library the core links for everyone else. Darwin
# serves math from libSystem and never noticed; a glibc host fails to resolve
# nan() at link time.
if(NOT WIN32)
    target_link_libraries(test_xrt_assertion PRIVATE m)
endif()
add_test(NAME test_xrt_assertion COMMAND $<TARGET_FILE:test_xrt_assertion>)
set_tests_properties(test_xrt_assertion PROPERTIES LABELS "aot;vm;assertion;shared-core")
add_test(NAME test_assertion_vm_equality
    COMMAND $<TARGET_FILE:xray> run
            ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/assertion/deep_equality_vm.xr)
set_tests_properties(test_assertion_vm_equality PROPERTIES
    LABELS "vm;assertion;shared-core")
add_test(NAME test_assertion_retired_names_are_ordinary
    COMMAND $<TARGET_FILE:xray> run
            ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/assertion/retired_names_are_ordinary.xr)
set_tests_properties(test_assertion_retired_names_are_ordinary PROPERTIES
    LABELS "frontend;vm;assertion;removed-surface")
add_test(NAME test_assertion_same_t_contextual
    COMMAND $<TARGET_FILE:xray> run
            ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/assertion/same_t_contextual.xr)
set_tests_properties(test_assertion_same_t_contextual PROPERTIES
    LABELS "frontend;vm;assertion;type-system")
add_test(NAME test_array_growth_accounting
    COMMAND $<TARGET_FILE:xray> run
            ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/runtime/array_growth_accounting.xr)
set_tests_properties(test_array_growth_accounting PROPERTIES
    LABELS "vm;runtime;memory")
add_executable(test_xrt_assertion_freestanding aot/test_xrt_assertion_freestanding.c)
target_compile_definitions(test_xrt_assertion_freestanding PRIVATE
    XRAY_PROFILE_FREESTANDING=1)
target_include_directories(test_xrt_assertion_freestanding PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include
)
add_test(NAME test_xrt_assertion_freestanding
    COMMAND $<TARGET_FILE:test_xrt_assertion_freestanding>)
set_tests_properties(test_xrt_assertion_freestanding PROPERTIES
    LABELS "aot;freestanding;assertion;shared-core")
add_executable(test_xrt_type_identity_freestanding aot/test_xrt_type_identity_freestanding.c)
target_compile_definitions(test_xrt_type_identity_freestanding PRIVATE
    XRAY_PROFILE_FREESTANDING=1)
target_include_directories(test_xrt_type_identity_freestanding PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include
)
add_test(NAME test_xrt_type_identity_freestanding
    COMMAND $<TARGET_FILE:test_xrt_type_identity_freestanding>)
# This test deliberately compiles the header-only AOT runtime with XRT_IMPL and
# custom allocators.  Linking xray_core would give it a second runtime-global
# owner and fails deterministically under sanitizer linkers.
add_executable(test_xrt_user_hash_eq aot/test_xrt_user_hash_eq.c)
target_compile_definitions(test_xrt_user_hash_eq PRIVATE XR_TEST_HAS_PROCESS_SHUTDOWN=1)
target_include_directories(test_xrt_user_hash_eq PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include
)
add_test(NAME test_xrt_user_hash_eq COMMAND $<TARGET_FILE:test_xrt_user_hash_eq>)

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
add_xray_unit_test(test_xi_pass_policy ir/test_xi_pass_policy.c)
add_xray_unit_test(test_xi_licm ir/test_xi_licm.c)
add_xray_unit_test(test_xi_ifconv ir/test_xi_ifconv.c)
add_xray_unit_test(test_xi_sccp ir/test_xi_sccp.c)
add_xray_unit_test(test_xi_gvn_pre ir/test_xi_gvn_pre.c)
add_xray_unit_test(test_xi_strength ir/test_xi_strength.c)
add_xray_unit_test(test_xi_dom_cache ir/test_xi_dom_cache.c)
add_xray_unit_test(test_xi_ivsr ir/test_xi_ivsr.c)
add_xray_unit_test(test_xi_inline ir/test_xi_inline.c)
add_xray_unit_test(test_xi_tail_call ir/test_xi_tail_call.c)
add_xray_unit_test(test_xi_verify_ext ir/test_xi_verify_ext.c)
add_xray_unit_test(test_xi_cleanup ir/test_xi_cleanup.c)
add_library(xi_allocation_probe OBJECT ${CMAKE_SOURCE_DIR}/src/ir/xi.c)
target_include_directories(xi_allocation_probe PRIVATE ${XRAY_COMMON_INCLUDES})
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC"
        OR (CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    target_compile_options(xi_allocation_probe PRIVATE
        /W4 /WX "/FI${CMAKE_CURRENT_SOURCE_DIR}/program/xr_program_allocation_probe.h")
else()
    target_compile_options(xi_allocation_probe PRIVATE
        -Wall -Wextra -Werror -include "${CMAKE_CURRENT_SOURCE_DIR}/program/xr_program_allocation_probe.h")
endif()
add_xray_unit_test(test_xi_allocations ir/test_xi_allocations.c
    $<TARGET_OBJECTS:xi_allocation_probe>)
set_tests_properties(test_xi_allocations PROPERTIES LABELS "unit;ir;allocation-failure;ownership")
add_xray_unit_test(test_xi_cleanup_lower ir/test_xi_cleanup_lower.c)
add_xray_unit_test(test_xi_cleanup_clone ir/test_xi_cleanup_clone.c)
add_xray_unit_test(test_xi_cleanup_integration ir/test_xi_cleanup_integration.c)
add_xray_unit_test(test_xi_shuffle ir/test_xi_shuffle.c)

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
add_xray_unit_test(test_process_shutdown base/test_process_shutdown.c)
set_tests_properties(test_process_shutdown PROPERTIES LABELS "runtime;memory;task-218")

# ============================================================================
# coro/ — MPSC queue, work-stealing queue
# ============================================================================

add_xray_unit_test(test_mpsc_queue coro/test_mpsc_queue.c)
add_xray_unit_test(test_steal_queue coro/test_steal_queue.c)
add_xray_unit_test(test_async_pool coro/test_async_pool.c)
add_library(async_allocation_probe OBJECT ${CMAKE_SOURCE_DIR}/src/coro/xasync.c)
target_include_directories(async_allocation_probe PRIVATE ${XRAY_COMMON_INCLUDES})
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC"
        OR (CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    target_compile_options(async_allocation_probe PRIVATE
        /W4 /WX "/FI${CMAKE_CURRENT_SOURCE_DIR}/program/xr_program_allocation_probe.h")
else()
    target_compile_options(async_allocation_probe PRIVATE
        -Wall -Wextra -Werror -include "${CMAKE_CURRENT_SOURCE_DIR}/program/xr_program_allocation_probe.h")
endif()
add_xray_unit_test(test_async_allocations coro/test_async_allocations.c
    $<TARGET_OBJECTS:async_allocation_probe>)
set_tests_properties(test_async_pool test_async_allocations PROPERTIES
    LABELS "unit;runtime;async;ownership")
add_xray_unit_test(test_scheduler_runq coro/test_scheduler_runq.c)
add_xray_unit_test(test_timer_wheel coro/test_timer_wheel.c)
add_executable(test_channel_buffer runtime/test_channel_buffer.c)
target_include_directories(test_channel_buffer PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_channel_buffer COMMAND $<TARGET_FILE:test_channel_buffer>)
set_tests_properties(test_channel_buffer PROPERTIES LABELS "unit;runtime;channel;shared-core")
add_xray_bootstrap_unit_test(test_channel_storage runtime/test_channel_storage.c)
set_tests_properties(test_channel_storage PROPERTIES LABELS "unit;runtime;channel;shared-core;ownership")
# This scheduler fixture consumes runtime state, not stage-2 stdlib overlays.
add_xray_bootstrap_unit_test(test_channel_close coro/test_channel_close.c)
add_xray_unit_test(test_scope_wait coro/test_scope_wait.c)
add_xray_unit_test(test_work_queue_wait coro/test_work_queue_wait.c)
add_xray_unit_test(test_result_group coro/test_result_group.c)
add_xray_unit_test(test_native_backend coro/test_native_backend.c)
if(MSVC OR CMAKE_C_SIMULATE_ID STREQUAL "MSVC")
    # These white-box scheduler fixtures embed the complete isolate/runtime
    # state and exceed the PE default 1 MiB stack before the first assertion.
    foreach(target IN ITEMS
            test_scheduler_runq
            test_channel_close
            test_scope_wait
            test_work_queue_wait
            test_result_group
            test_native_backend)
        target_link_options(${target} PRIVATE "LINKER:/STACK:16777216")
    endforeach()
endif()

# ============================================================================
# module/ — Resolver, semver, lockfile, source cache
# ============================================================================

add_xray_unit_test(test_resolver module/test_resolver.c)
if(NOT WIN32)
  add_xray_unit_test(test_module_resolver module/test_module_resolver.c)
endif()
add_xray_unit_test(test_module_graph module/test_module_graph.c)
add_xray_unit_test(test_module_publication module/test_module_publication.c)
add_xray_unit_test(test_module_identity module/test_module_identity.c)
target_compile_definitions(test_module_identity PRIVATE
    XRAY_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
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
add_xray_unit_test(test_target_simd_core stdlib/test_target_simd_core.c)
add_xray_unit_test(test_slice_window_core stdlib/test_slice_window_core.c)
add_executable(test_xrt_compare_owner_freestanding
    aot/test_xrt_compare_owner_freestanding.c)
target_include_directories(test_xrt_compare_owner_freestanding PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)
add_test(NAME test_xrt_compare_owner_freestanding
    COMMAND $<TARGET_FILE:test_xrt_compare_owner_freestanding>)
add_xray_unit_test(test_range_core stdlib/test_range_core.c)
add_executable(test_data_pointer_core stdlib/test_data_pointer_core.c)
target_include_directories(test_data_pointer_core PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_CURRENT_SOURCE_DIR}
)
add_test(NAME test_data_pointer_core COMMAND $<TARGET_FILE:test_data_pointer_core>)
add_executable(test_xrt_data_pointer_owner_freestanding
    aot/test_xrt_data_pointer_owner_freestanding.c)
target_include_directories(test_xrt_data_pointer_owner_freestanding PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)
add_test(NAME test_xrt_data_pointer_owner_freestanding
    COMMAND $<TARGET_FILE:test_xrt_data_pointer_owner_freestanding>)
# The C90 half of the same family. It was written, listed as a fixture in
# contracts/semantic-owners.toml, and registered nowhere -- so the one thing it
# proves, that the restricted data-pointer adapter compiles and runs under
# strict ISO C90, was proved by nobody. Its null_test and byte_array_copy
# siblings are registered below; check_semantic_owners.py even hardcodes a
# requirement that the byte_array_copy one stays registered, and this file was
# simply left out of that tuple.
#
# Not add_xray_unit_test(): that macro links xray_core and does not set the
# standard, and the whole value of a C90 KAT is compiling without either.
if(NOT WIN32)
    add_executable(test_xrt_data_pointer_owner_c90 aot/test_xrt_data_pointer_owner_c90.c)
    target_include_directories(test_xrt_data_pointer_owner_c90 PRIVATE
        ${XRAY_COMMON_INCLUDES}
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_target_properties(test_xrt_data_pointer_owner_c90 PROPERTIES C_STANDARD 90 C_EXTENSIONS OFF)
    add_test(NAME test_xrt_data_pointer_owner_c90
        COMMAND $<TARGET_FILE:test_xrt_data_pointer_owner_c90>)
endif()
add_executable(test_cell_access_core stdlib/test_cell_access_core.c)
target_include_directories(test_cell_access_core PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)
add_test(NAME test_cell_access_core COMMAND $<TARGET_FILE:test_cell_access_core>)
add_executable(test_xrt_cell_access_owner_freestanding
    aot/test_xrt_cell_access_owner_freestanding.c)
target_include_directories(test_xrt_cell_access_owner_freestanding PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)
add_test(NAME test_xrt_cell_access_owner_freestanding
    COMMAND $<TARGET_FILE:test_xrt_cell_access_owner_freestanding>)
add_xray_unit_test(test_xrt_range_owner_freestanding aot/test_xrt_range_owner_freestanding.c)
add_executable(test_enum_metadata_core stdlib/test_enum_metadata_core.c)
target_include_directories(test_enum_metadata_core PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_CURRENT_SOURCE_DIR}
)
add_test(NAME test_enum_metadata_core COMMAND $<TARGET_FILE:test_enum_metadata_core>)
add_executable(test_xrt_enum_metadata_owner_freestanding
    aot/test_xrt_enum_metadata_owner_freestanding.c)
target_include_directories(test_xrt_enum_metadata_owner_freestanding PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)
add_test(NAME test_xrt_enum_metadata_owner_freestanding
    COMMAND $<TARGET_FILE:test_xrt_enum_metadata_owner_freestanding>)
add_xray_unit_test(test_xrt_numeric_neg_owner_freestanding aot/test_xrt_numeric_neg_owner_freestanding.c)
add_xray_unit_test(test_xrt_null_test_owner_freestanding aot/test_xrt_null_test_owner_freestanding.c)
if(NOT WIN32)
    add_executable(test_xrt_null_test_owner_c90 aot/test_xrt_null_test_owner_c90.c)
    target_include_directories(test_xrt_null_test_owner_c90 PRIVATE
        ${XRAY_COMMON_INCLUDES}
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_target_properties(test_xrt_null_test_owner_c90 PROPERTIES C_STANDARD 90 C_EXTENSIONS OFF)
    add_test(NAME test_xrt_null_test_owner_c90 COMMAND $<TARGET_FILE:test_xrt_null_test_owner_c90>)
endif()
add_xray_unit_test(test_xrt_byte_slice_compare_owner_freestanding aot/test_xrt_byte_slice_compare_owner_freestanding.c)
add_xray_unit_test(test_xrt_byte_array_copy_owner_freestanding aot/test_xrt_byte_array_copy_owner_freestanding.c)
add_xray_unit_test(test_xrt_raw_scalar_owner_freestanding aot/test_xrt_raw_scalar_owner_freestanding.c)
if(NOT WIN32)
    add_executable(test_xrt_byte_array_copy_owner_c90 aot/test_xrt_byte_array_copy_owner_c90.c)
    target_include_directories(test_xrt_byte_array_copy_owner_c90 PRIVATE
        ${XRAY_COMMON_INCLUDES}
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_target_properties(test_xrt_byte_array_copy_owner_c90 PROPERTIES C_STANDARD 90 C_EXTENSIONS OFF)
    add_test(NAME test_xrt_byte_array_copy_owner_c90
        COMMAND $<TARGET_FILE:test_xrt_byte_array_copy_owner_c90>)
endif()
add_xray_unit_test(test_string_core stdlib/test_string_core.c)
add_xray_unit_test(test_strbuf_core stdlib/test_strbuf_core.c)
add_xray_unit_test(test_truthy_core stdlib/test_truthy_core.c)
add_xray_unit_test(test_type_identity_core stdlib/test_type_identity_core.c)
add_xray_unit_test(test_io_core stdlib/test_io_core.c)
add_xray_unit_test(test_os_core stdlib/test_os_core.c)
add_xray_unit_test(test_crypto stdlib/test_crypto.c)
if(XR_STDLIB_COMPRESS)
    add_xray_unit_test(test_compress stdlib/test_compress.c)
endif()
add_xray_unit_test(test_xml_node stdlib/test_xml_node.c)
add_xray_unit_test(test_prelude_init stdlib/test_prelude_init.c)
set_property(TEST test_prelude_init test_xi_emit test_xi_encoding_slice_native
    APPEND PROPERTY FIXTURES_SETUP prelude_enum_identity)
add_xray_unit_test(test_native_type_surface stdlib/test_native_type_surface.c)
set_tests_properties(test_native_type_surface PROPERTIES LABELS "stdlib;metadata;native-types")
if(XRAY_STDLIB_VM_FASTPATHS)
    add_xray_unit_test(test_stdlib_vm_fastpath_abi stdlib/test_stdlib_vm_fastpath_abi.c)
    set_tests_properties(test_stdlib_vm_fastpath_abi PROPERTIES
        LABELS "stdlib;performance;abi;task-256"
        RUN_SERIAL TRUE
        TIMEOUT 120
    )
    add_test(
        NAME test_generate_vm_fastpaths
        COMMAND ${XRAY_PYTHON}
                ${CMAKE_CURRENT_SOURCE_DIR}/stdlib/test_generate_vm_fastpaths.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(test_generate_vm_fastpaths PROPERTIES
        LABELS "stdlib;metadata;tooling;task-256"
    )
endif()
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

add_xray_unit_test(test_api_defense api/test_api_defense.c)
add_xray_unit_test(test_dostring_module_defaults api/test_dostring_module_defaults.c)
set_tests_properties(test_dostring_module_defaults PROPERTIES LABELS "api;modules;defaults")
add_xray_unit_test(test_dostring_program_image api/test_dostring_program_image.c)
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

# test_repl needs the isolate-profile factory (xr_isolate_profile_*)
# in addition to xray_core, so build it like test_cli_spec with an
# explicit source list.
add_executable(test_repl
    api/test_repl.c
    ${CMAKE_SOURCE_DIR}/src/api/xisolate_profile.c
)
target_include_directories(test_repl PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/api
)
target_link_libraries(test_repl PRIVATE xray_core)
add_test(NAME test_repl COMMAND test_repl)

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
        ${CMAKE_SOURCE_DIR}/src/runtime/object
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

    file(GLOB LSP_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/app/lsp/*.c)
    list(REMOVE_ITEM LSP_SOURCES ${CMAKE_SOURCE_DIR}/src/app/lsp/xlsp_main.c)
    # Module-graph authority is shared with CLI source commands and is not part
    # of xray_core. Production xray already links these sources; full-source
    # LSP unit binaries must name them as well.
    list(APPEND LSP_SOURCES
        ${CMAKE_SOURCE_DIR}/src/app/cli/xcli_graph_authority.c
        ${CMAKE_SOURCE_DIR}/src/app/cli/xcli_fs.c
        ${CMAKE_SOURCE_DIR}/src/app/cli/xcli_output.c)

    # The full-source LSP tests exercise the same Windows CRT-pipe adapters as
    # the production server; keep them enabled on every host.
    add_executable(test_lsp_document
        lsp/test_lsp_document.c
        ${LSP_SOURCES}
    )
    target_include_directories(test_lsp_document PRIVATE
        ${XRAY_COMMON_INCLUDES}
        ${CMAKE_SOURCE_DIR}/src/app/lsp
        ${CMAKE_SOURCE_DIR}/src/app/cli
        ${CMAKE_SOURCE_DIR}/src/frontend/format
    )
    target_link_libraries(test_lsp_document PRIVATE xray_core)
    add_test(NAME test_lsp_document COMMAND test_lsp_document)
    add_test(NAME test_lsp_protocol_transcript
        COMMAND ${XRAY_PYTHON}
            ${CMAKE_SOURCE_DIR}/tests/lsp/test_protocol_transcript.py
            --xray $<TARGET_FILE:xray>
    )
    set_tests_properties(test_lsp_protocol_transcript PROPERTIES TIMEOUT 20)

    add_executable(test_lsp_config
        lsp/test_lsp_config.c
        ${LSP_SOURCES}
    )
    target_include_directories(test_lsp_config PRIVATE
        ${XRAY_COMMON_INCLUDES}
        ${CMAKE_SOURCE_DIR}/src/app/lsp
        ${CMAKE_SOURCE_DIR}/src/app/cli
        ${CMAKE_SOURCE_DIR}/src/frontend/format
    )
    target_link_libraries(test_lsp_config PRIVATE xray_core)
    add_test(NAME test_lsp_config COMMAND test_lsp_config)

    add_executable(test_lsp_navigation
        lsp/test_lsp_navigation.c
        ${LSP_SOURCES}
    )
    target_include_directories(test_lsp_navigation PRIVATE
        ${XRAY_COMMON_INCLUDES}
        ${CMAKE_SOURCE_DIR}/src/app/lsp
        ${CMAKE_SOURCE_DIR}/src/app/cli
        ${CMAKE_SOURCE_DIR}/src/frontend/format
    )
    target_link_libraries(test_lsp_navigation PRIVATE xray_core)
    add_test(NAME test_lsp_navigation COMMAND test_lsp_navigation)

    add_executable(test_lsp_diagnostics
        lsp/test_lsp_diagnostics.c
        ${LSP_SOURCES}
    )
    target_include_directories(test_lsp_diagnostics PRIVATE
        ${XRAY_COMMON_INCLUDES}
        ${CMAKE_SOURCE_DIR}/src/app/lsp
        ${CMAKE_SOURCE_DIR}/src/app/cli
        ${CMAKE_SOURCE_DIR}/src/frontend/format
    )
    target_link_libraries(test_lsp_diagnostics PRIVATE xray_core)
    add_test(NAME test_lsp_diagnostics COMMAND test_lsp_diagnostics)
endif()

# ============================================================================
# dap/ — Debug Adapter Protocol (conditional)
# ============================================================================

if(XR_BUILD_DAP)
    add_executable(test_dap_server
        dap/test_dap_server.c
        ${CMAKE_SOURCE_DIR}/src/base/xjson.c
        ${CMAKE_SOURCE_DIR}/src/base/xutf8.c
    )
    target_include_directories(test_dap_server PRIVATE
        ${CMAKE_SOURCE_DIR}/src/base
        ${CMAKE_SOURCE_DIR}/src/app/dap
        ${CMAKE_SOURCE_DIR}/src/runtime/object
    )
    add_test(NAME test_dap_server COMMAND test_dap_server)

    add_executable(test_dap_breakpoints
        dap/test_dap_breakpoints.c
    )
    target_include_directories(test_dap_breakpoints PRIVATE
        ${CMAKE_SOURCE_DIR}/src/app/dap
        ${CMAKE_SOURCE_DIR}/include
    )
    add_test(NAME test_dap_breakpoints COMMAND test_dap_breakpoints)

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
    add_executable(test_mcp_transport
        mcp/test_mcp_transport.c
        ${CMAKE_SOURCE_DIR}/src/app/mcp/xmcp_transport_stdio.c
    )
    target_include_directories(test_mcp_transport PRIVATE
        ${XRAY_COMMON_INCLUDES}
        ${CMAKE_SOURCE_DIR}/src/app/mcp
        ${CMAKE_SOURCE_DIR}/src/base
    )
    target_link_libraries(test_mcp_transport PRIVATE xray_core)
    add_test(NAME test_mcp_transport COMMAND test_mcp_transport)

    add_executable(test_mcp_protocol
        mcp/test_mcp_protocol.c
        ${CMAKE_SOURCE_DIR}/src/app/mcp/xmcp_jsonrpc.c
        ${CMAKE_SOURCE_DIR}/src/app/mcp/xmcp_protocol.c
        ${CMAKE_SOURCE_DIR}/src/app/mcp/xmcp_registry.c
        ${CMAKE_SOURCE_DIR}/src/app/mcp/xmcp_tools.c
        ${CMAKE_SOURCE_DIR}/src/app/mcp/xmcp_tools_schema.c
        ${CMAKE_SOURCE_DIR}/src/app/mcp/xmcp_tools_lang.c
        ${CMAKE_SOURCE_DIR}/src/app/mcp/xmcp_tools_run.c
        ${CMAKE_SOURCE_DIR}/src/app/mcp/xmcp_tools_kb.c
        ${CMAKE_SOURCE_DIR}/src/app/mcp/xmcp_resources.c
        ${CMAKE_SOURCE_DIR}/src/app/mcp/xmcp_prompts.c
        ${CMAKE_SOURCE_DIR}/src/app/mcp/xmcp_knowledge.c
        ${CMAKE_SOURCE_DIR}/src/app/mcp/xmcp_knowledge_generated.c
        ${CMAKE_SOURCE_DIR}/src/base/xjson.c
        ${CMAKE_SOURCE_DIR}/src/base/xutf8.c
    )
    target_include_directories(test_mcp_protocol PRIVATE
        ${XRAY_COMMON_INCLUDES}
        ${CMAKE_SOURCE_DIR}/src/app/mcp
        ${CMAKE_SOURCE_DIR}/src/base
        ${CMAKE_SOURCE_DIR}/src/runtime/object
    )
    target_link_libraries(test_mcp_protocol PRIVATE xray_core)
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
        knowledge
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

add_executable(test_cli_fs
    cli/test_cli_fs.c
    ${CMAKE_SOURCE_DIR}/src/app/cli/xcli_fs.c
    ${CMAKE_SOURCE_DIR}/src/app/cli/xcli_output.c
)
target_include_directories(test_cli_fs PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_SOURCE_DIR}/src/app/cli
)
target_link_libraries(test_cli_fs PRIVATE xray_core)
add_test(NAME test_cli_fs COMMAND test_cli_fs)

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
    ${CMAKE_SOURCE_DIR}/src/app/cli/xcli_fs.c
    ${CMAKE_SOURCE_DIR}/src/app/cli/xcli_output.c
)
target_include_directories(test_cli_spec PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_BINARY_DIR}/generated
    ${CMAKE_SOURCE_DIR}/src/app/cli
)
target_link_libraries(test_cli_spec PRIVATE xray_core)
add_test(NAME test_cli_spec COMMAND test_cli_spec)

add_executable(test_cli_toolchain
    cli/test_cli_toolchain.c
    ${CMAKE_SOURCE_DIR}/src/app/toolchain/xtc_command.c
    ${CMAKE_SOURCE_DIR}/src/app/toolchain/xtc_model.c
    ${CMAKE_SOURCE_DIR}/src/app/toolchain/xtc_config.c
    ${CMAKE_SOURCE_DIR}/src/app/toolchain/xtc_discovery.c
    ${CMAKE_SOURCE_DIR}/src/app/toolchain/xtc_json.c
    ${CMAKE_SOURCE_DIR}/src/app/toolchain/xtc_probe_cache.c
    ${CMAKE_SOURCE_DIR}/src/app/toolchain/xtc_process.c
    ${CMAKE_SOURCE_DIR}/src/app/toolchain/xtc_runtime_manifest.c
)
target_include_directories(test_cli_toolchain PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_SOURCE_DIR}/src/app/toolchain
)
target_link_libraries(test_cli_toolchain PRIVATE xray_core)
add_test(NAME test_cli_toolchain COMMAND test_cli_toolchain)

add_xray_unit_test(test_target_profile_authority
    cli/test_target_profile_authority.c
    ${CMAKE_SOURCE_DIR}/src/app/toolchain/xtc_model.c
    ${CMAKE_SOURCE_DIR}/src/app/toolchain/xtc_target_profile.c
)
set_tests_properties(test_target_profile_authority PROPERTIES
    LABELS "aot;runtime;target-machine")

add_executable(process_spawn_benchmark EXCLUDE_FROM_ALL
    ${CMAKE_SOURCE_DIR}/tests/benchmarks/process/process_spawn_benchmark.c
)
target_include_directories(process_spawn_benchmark PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(process_spawn_benchmark PRIVATE xray_core)

# ============================================================================
# test_harness/ — xray test runner self-tests
# Verify that @test discover, execution, pass/fail/skip, and exit codes
# all behave correctly.  These run the xray binary, not C unit tests.
# ============================================================================

set(_HARNESS_DIR ${CMAKE_SOURCE_DIR}/tests/test_harness)

add_test(NAME harness_single_pass
         COMMAND $<TARGET_FILE:xray> test ${_HARNESS_DIR}/single_pass.xr)
add_test(NAME harness_multi_pass
         COMMAND $<TARGET_FILE:xray> test ${_HARNESS_DIR}/multi_pass.xr)
add_test(NAME harness_with_skip
         COMMAND $<TARGET_FILE:xray> test ${_HARNESS_DIR}/with_skip.xr)

add_test(NAME harness_single_fail
         COMMAND $<TARGET_FILE:xray> test ${_HARNESS_DIR}/single_fail.xr)
set_tests_properties(harness_single_fail PROPERTIES WILL_FAIL TRUE)

add_test(NAME harness_no_tests
         COMMAND $<TARGET_FILE:xray> test ${_HARNESS_DIR}/no_tests.xr)
set_tests_properties(harness_no_tests PROPERTIES WILL_FAIL TRUE)

add_test(NAME harness_mixed_pass_fail
         COMMAND $<TARGET_FILE:xray> test ${_HARNESS_DIR}/mixed_pass_fail.xr)
set_tests_properties(harness_mixed_pass_fail PROPERTIES WILL_FAIL TRUE)

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

add_test(NAME test_native_buffer_storage
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/cli/test_native_buffer_storage.py
        --compiler $<TARGET_FILE:xray_stdlib_bcgen> ${XR_NATIVE_SOURCE_HOST_ARGS})
set_tests_properties(test_native_buffer_storage PROPERTIES
    LABELS "aot;generated-c;ownership" TIMEOUT 120)

xray_prepend_windows_asan_runtime_to_directory_tests()
xray_attach_contract_assertions()

add_test(NAME test_native_leaf_representation
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/cli/test_native_leaf_representation.py
        --compiler $<TARGET_FILE:xray_stdlib_bcgen> ${XR_NATIVE_SOURCE_HOST_ARGS})
set_tests_properties(test_native_leaf_representation PROPERTIES
    LABELS "aot;generated-c;integration" TIMEOUT 180)

add_test(NAME test_native_raw_slice
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/cli/test_native_raw_slice.py
        --compiler $<TARGET_FILE:xray_stdlib_bcgen> ${XR_NATIVE_SOURCE_HOST_ARGS})
set_tests_properties(test_native_raw_slice PROPERTIES
    LABELS "aot;generated-c;ownership" TIMEOUT 120)
add_test(NAME test_cgen_raw_slice COMMAND $<TARGET_FILE:test_xi_cgen> raw-slice)
set_tests_properties(test_cgen_raw_slice PROPERTIES LABELS "aot;ownership" TIMEOUT 120)
