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


add_subdirectory(xir/bounded_call)
add_subdirectory(shared/native_admission)
add_subdirectory(shared/bits_wide)
add_subdirectory(shared/core_intrinsic_owner)


add_subdirectory(module/declaration_owner)
add_subdirectory(module/stdlib_source_owner)
if(WIN32)
    add_subdirectory(os/thread_join_owner)
    add_subdirectory(module/module_owner)
    add_subdirectory(module/project_owner)

endif()
include(${CMAKE_CURRENT_SOURCE_DIR}/cli/xir_target_authority.cmake)



include(${CMAKE_CURRENT_SOURCE_DIR}/cli/toolchain_config.cmake)
add_subdirectory(cli/pe_version_owner)
include(${CMAKE_CURRENT_SOURCE_DIR}/cli/workspace_owner.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cli/source_paths_owner.cmake)

include(${CMAKE_CURRENT_SOURCE_DIR}/cli/check_owner.cmake)








include(${CMAKE_CURRENT_SOURCE_DIR}/base/windows_path_owner.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/cgen_bytes_owner.cmake)


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

set(XIR_EFFECT_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_effect_fixture.c)

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

include(${CMAKE_CURRENT_SOURCE_DIR}/xir/allocation_feedback.cmake)

set(XIR_LOCAL_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_local_fixture.c)



add_executable(test_xir_checked xir/test_xir_checked.c)
target_link_libraries(test_xir_checked PRIVATE xray_xir)
add_executable(test_xir_checked_allocations xir/test_xir_checked_allocations.c
    ${CMAKE_SOURCE_DIR}/src/base/xsha256.c
    ${CMAKE_SOURCE_DIR}/src/shared/xnative_declaration.c)
target_link_libraries(test_xir_checked_allocations PRIVATE xray_compile_resources)
target_include_directories(test_xir_checked_allocations PRIVATE ${CMAKE_SOURCE_DIR}/src)






set(XIR_CALL_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_call_fixture.c)




include("${CMAKE_CURRENT_SOURCE_DIR}/xir/calls_owned_receive.cmake")
set(XIR_SCALAR_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_scalar_fixture.c)

# Complete panic outcomes keep message ownership independent of result values.

# These implementations are included under the runtime allocation macros,
# never compiled as additional uncounted translation units.
set_property(SOURCE xir/test_xir_panic_carrier.c APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/xir/xxir_task_budget.c"
    "${CMAKE_SOURCE_DIR}/src/xir/xxir_task.c"
    "${CMAKE_SOURCE_DIR}/src/xir/xxir_task_outcome.inc.c")

add_test(NAME test_xir_panic_carrier_wire_vector COMMAND ${XRAY_PYTHON}
    ${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_panic_carrier_vector.py
    --output ${CMAKE_CURRENT_BINARY_DIR}/generated/panic-carrier-wire-vector.json)
set_tests_properties(test_xir_panic_carrier_wire_vector PROPERTIES LABELS "unit;xir;ownership;abi")

include("${CMAKE_CURRENT_LIST_DIR}/xir/object_storage_prefix.cmake")
add_executable(test_xir_values xir/test_xir_values.c)
target_link_libraries(test_xir_values PRIVATE xray_xir_scalar)



set(XIR_PROGRAM_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_program_fixture.c)




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

set(XIR_STRING_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_string_fixture.c)

include("${CMAKE_CURRENT_SOURCE_DIR}/xir/string_allocation_owner.cmake")

set(XIR_OUTPUT_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_output_fixture.c)




add_test(NAME test_xir_source_product_identity_oracle COMMAND ${XRAY_PYTHON}
    ${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_source_product_identity.py)
set_tests_properties(test_xir_source_product_identity_oracle PROPERTIES LABELS "unit;xir;abi;ownership" TIMEOUT 30)
set(XIR_SOURCE_OWNER_FIXTURE "${CMAKE_SOURCE_DIR}/tests/fixtures/xir_compile_owner")
set(XIR_SOURCE_OWNER_NATIVE "${CMAKE_CURRENT_BINARY_DIR}/xir-source-owner-native.c")





set(XIR_PACKAGE_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_package_fixture.c)









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

set(XIR_SOURCE_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_source_fixture.c)
set(XIR_CHECKED_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_source_fixture.xrc)











include(${CMAKE_CURRENT_SOURCE_DIR}/xir/enum_description.cmake)

include(${CMAKE_CURRENT_SOURCE_DIR}/xir/class_hierarchy_contract.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/class_move_shape.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/class_stage_contract.cmake)





include(${CMAKE_CURRENT_SOURCE_DIR}/xir/intrinsic_registry.cmake)




include(${CMAKE_CURRENT_SOURCE_DIR}/xir/assert_condition.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/assert_panics.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/assert_equal.cmake)

include(${CMAKE_CURRENT_SOURCE_DIR}/xir/nullable.cmake)






include(${CMAKE_CURRENT_SOURCE_DIR}/xir/class_array_fields.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/dependency_ready.cmake)




include(${CMAKE_CURRENT_SOURCE_DIR}/xir/nested_nullable.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/semantic60_transition.cmake)


include(${CMAKE_CURRENT_SOURCE_DIR}/xir/array_reorder_owner.cmake)



set(XIR_ARRAY_CHECKED_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_array_source.xrc)
set(XIR_ARRAY_BOUNDS_CHECKED_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_array_bounds.xrc)
set(XIR_ARRAY_REORDER_CHECKED_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_array_reorder.xrc)





set(XIR_FLOAT_CHECKED_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_float_source.xrc)
set(XIR_FLOAT_SOURCE_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_float_source.c)


include(xir/mixed_lifecycle.cmake)
include(xir/mixed_terminals.cmake)
include(xir/mixed_observation.cmake)


include(xir/mixed_root_permissions.cmake)








set(XIR_PANIC_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_panic_source.c)



foreach(xir_test IN ITEMS  test_xir_checked test_xir_checked_allocations    test_xir_values test_xir_value_allocations          )
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



set(XR_NATIVE_SOURCE_HOST_ARGS --host-compiler ${CMAKE_C_COMPILER})
if(MSVC)
    list(APPEND XR_NATIVE_SOURCE_HOST_ARGS --msvc)
endif()
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    list(APPEND XR_NATIVE_SOURCE_HOST_ARGS --msvc-atomics)
endif()










add_test(NAME test_enum_namespace_execution
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/tests/cli/run_enum_namespace_tests.py
        --compiler $<TARGET_FILE:xray>)
set_tests_properties(test_enum_namespace_execution PROPERTIES
    LABELS "vm;aot;enum;integration" TIMEOUT 300)













set(XR_LEAF_PRODUCT_NATIVE_SOURCE
    ${CMAKE_CURRENT_BINARY_DIR}/generated/leaf_product_native.c)

set_source_files_properties(${XR_LEAF_PRODUCT_NATIVE_SOURCE}
    PROPERTIES GENERATED TRUE)



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



include(xir/atomic_native20.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/array_generated.cmake)


include(${CMAKE_CURRENT_SOURCE_DIR}/xir/fixed_bytes.cmake)








include(${CMAKE_CURRENT_SOURCE_DIR}/xir/crypto_native.cmake)

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
target_link_libraries(test_cgen_verify_output PRIVATE xray_lir_c)
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
  target_link_libraries(test_module_resolver PRIVATE xray_xir_source)
endif()

add_xray_unit_test(test_source_cache module/test_source_cache.c)

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
include("${CMAKE_CURRENT_LIST_DIR}/base/file_owned.cmake")
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


# Include every test, even when new declarations are appended later.
cmake_language(DEFER CALL xray_prepend_windows_asan_runtime_to_directory_tests)

# Compile the actual Source coordinators with an allocation-only test boundary.

# All production consumers still use the same fresh core. Only these two
# exact owners receive the test allocator; no production test-mode branch.

add_test(NAME test_callable52_wire_encoder
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/plan/derive_callable52_wire.py)

# Exercise the original publication obligation against actual Source owners.

include(${CMAKE_CURRENT_SOURCE_DIR}/xir/constraint_queue.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/type_access_scratch.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/iterator_types.cmake)

include("${CMAKE_CURRENT_SOURCE_DIR}/xir/error_flow_scratch.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/xir/provenance_budget_release.cmake")
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/declaration_proof_scratch.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/type_match_scratch.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/signature_shape_scratch.cmake)

include(${CMAKE_CURRENT_SOURCE_DIR}/xir/tuple_owner.cmake)
include(xir/tuple_values.cmake)

include(xir/tuple_source_compile_owner.cmake)

# Fresh whole compiler/runtime fault matrices; scheduling does not extend deadlines.

include(xir/atomic_admission_owner.cmake)



include(xir/atomic_a2_retry.cmake)

include(xir/atomic_runtime_core.cmake)

include(xir/atomic_legacy_admission_owner.cmake)

include(xir/atomic_source_admission_owner.cmake)

include(module/module_graph_logical_source.cmake)

include(xir/atomic_a2_runtime_fault.cmake)

include(xir/semantic65_vectors.cmake)





include(xir/source_shadow_function.cmake)



include("${CMAKE_CURRENT_LIST_DIR}/xir/go_source.cmake")


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










include("${CMAKE_CURRENT_LIST_DIR}/xir/go_pending_normal.cmake")



include("${CMAKE_CURRENT_LIST_DIR}/xir/vm19_fault_runtime.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/xir/executor_first_failure.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/task_fault_order.cmake")



include("${CMAKE_CURRENT_LIST_DIR}/xir/task_unit.cmake")








include("${CMAKE_CURRENT_LIST_DIR}/xir/stdlib_resources.cmake")

include("${PROJECT_SOURCE_DIR}/tests/cli/xir_public_cli_product.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/stdlib_preparation.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/xir/task_outcome_copy_controls.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/xir/range_check.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/array_fill.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/array_concat.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/xir/task_late_cancel_cross.cmake")

include(xir/task_array_admission.cmake)

include("${CMAKE_CURRENT_LIST_DIR}/xir/byte_array_value.cmake")


add_executable(test_xir_array_capacity_core xir/test_xir_array_capacity_core.c)
target_include_directories(test_xir_array_capacity_core PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(test_xir_array_capacity_core PRIVATE xray_xir_scalar)

include("${CMAKE_CURRENT_LIST_DIR}/xir/p09_compile_domain.cmake")





foreach(capacity_core_target IN ITEMS test_xir_array_capacity_core)
    if(MSVC)
        target_compile_options(${capacity_core_target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${capacity_core_target} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${capacity_core_target} COMMAND ${capacity_core_target})
    set_tests_properties(${capacity_core_target} PROPERTIES TIMEOUT 120 LABELS "unit;xir;array;ownership;budget")
endforeach()


include("${CMAKE_CURRENT_LIST_DIR}/xir/array_capacity_budget.cmake")



include("${CMAKE_CURRENT_LIST_DIR}/xir/source_layout22.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/xir/root_effects.cmake")



include("${CMAKE_CURRENT_LIST_DIR}/xir/root_cause_trace.cmake")







include(xir/callable_root_bounds.cmake)

include(xir/callable_root_source.cmake)
include(xir/source_copy_refinement.cmake)
include(xir/invocation_zero_inputs.cmake)
include(xir/effect_term_scratch.cmake)
include(xir/refinement_context_lifetime.cmake)



include(xir/callable70_checked_vectors.cmake)

include(xir/root_identity_vm_compiler_resources.cmake)

include(xir/instance_bindings.cmake)

include(xir/owned_string_source_resources.cmake)



include("${CMAKE_CURRENT_LIST_DIR}/xir/error_cell_index.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/xir/error_edge_snapshot.cmake")
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/program_call_permissions.cmake)












include(${CMAKE_CURRENT_SOURCE_DIR}/xir/root_parameters.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/root_parameter_vm.cmake)


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

include("${CMAKE_CURRENT_LIST_DIR}/xir/json_syntax.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/json_scalar_object.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/json_integer_object.cmake")





include("${CMAKE_CURRENT_LIST_DIR}/xir/source_decimal_parse.cmake")











include(xir/cell_owner_source.cmake)



include(${CMAKE_CURRENT_SOURCE_DIR}/xir/vm_bind_table.cmake)












include("${CMAKE_CURRENT_LIST_DIR}/xir/owned_instruction_inventory.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/move_values.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/xir/cfg_storage.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/cfg_dominance_order.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/xir/instance_domain_fuel.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/xir/specialize_key_controls.cmake")
include(${CMAKE_CURRENT_SOURCE_DIR}/xir/stdlib_families.cmake)

include(${CMAKE_CURRENT_SOURCE_DIR}/xir/program_correspondence.cmake)

add_subdirectory(xir/p10_reference)

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

include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_string_place.cmake)


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



add_test(NAME test_xir_combined_wire_smoke COMMAND test_xir_checked --wire-smoke)
set_tests_properties(test_xir_combined_wire_smoke PROPERTIES TIMEOUT 120 LABELS "unit;xir;checked;independent")

include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_graph_quiescence.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_keys.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_hash.cmake)

include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_hash_contains.cmake)


include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_map_index.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_hash_projection.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_set_algebra.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_map_value.cmake)

include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_hash_foreach.cmake)





include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_map_key_path.cmake)



include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_read_loan.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_view_types.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_view_origins.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_view_affine.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_view_liveness.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_view_roots.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_view_root_resolver.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_view_loan_origins.cmake)

include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_view_owner_path.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_call_views.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/xir/p09_local_call_views.cmake)

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

add_test(NAME test_xir_shared_interfaces_vectors COMMAND ${XRAY_PYTHON}
    ${CMAKE_CURRENT_LIST_DIR}/xir/derive_shared_interfaces_vectors.py)
set_tests_properties(test_xir_shared_interfaces_vectors PROPERTIES TIMEOUT 120 LABELS "unit;xir;checked;independent;ownership")
include("${CMAKE_CURRENT_LIST_DIR}/xir/never_type.cmake")











include("${CMAKE_CURRENT_LIST_DIR}/xir/source_local_recursive_binding.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/xir/source_records.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/xir/source_method_surface.cmake")



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





include("${CMAKE_CURRENT_LIST_DIR}/base/independent_tests.cmake")
