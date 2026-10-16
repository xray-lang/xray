# Root registers these gates after the complete production closure is reviewed.
set(XRAY_ROOT_CONDITIONAL_COMPILER_REPORTS_ROOT
    "${CMAKE_BINARY_DIR}/root-conditional-source-compiler" CACHE PATH
    "Exclusive reports root for complete conditional Source compiler faults")
foreach(_root_conditional_gate helpers source)
    add_executable(test_xir_root_conditional_${_root_conditional_gate}
        ${CMAKE_CURRENT_LIST_DIR}/test_xir_root_conditional_${_root_conditional_gate}.c)
    target_include_directories(test_xir_root_conditional_${_root_conditional_gate} PRIVATE ${XRAY_COMMON_INCLUDES})
    if(_root_conditional_gate STREQUAL "source")
        target_link_libraries(test_xir_root_conditional_${_root_conditional_gate} PRIVATE xray_xir_source)
    else()
        target_link_libraries(test_xir_root_conditional_${_root_conditional_gate} PRIVATE xray_xir xray_xir_scalar xray_xir_source)
    endif()
    if(MSVC)
        target_compile_options(test_xir_root_conditional_${_root_conditional_gate} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(test_xir_root_conditional_${_root_conditional_gate} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME test_xir_root_conditional_${_root_conditional_gate}
        COMMAND test_xir_root_conditional_${_root_conditional_gate})
    set_tests_properties(test_xir_root_conditional_${_root_conditional_gate} PROPERTIES
        LABELS "unit;xir;root-effects;higher-order;compiler;ownership" TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)
    # Observed Windows wall costs: complete MSVC 110s; ASan reaches its 600s cap.
    if(ENABLE_ASAN OR ENABLE_SANITIZERS)
        set(_root_source_compiler_cost 600.113)
    else()
        set(_root_source_compiler_cost 110.006)
    endif()
    if(WIN32 AND _root_conditional_gate STREQUAL "source")
        add_test(NAME test_xir_root_conditional_${_root_conditional_gate}_compiler
            COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/unit/xir/run_root_conditional_source_compiler.py"
                --executable "$<TARGET_FILE:test_xir_root_conditional_source>"
                --source-root "${PROJECT_SOURCE_DIR}"
                --reports-root "${XRAY_ROOT_CONDITIONAL_COMPILER_REPORTS_ROOT}" --workers 8)
        set_tests_properties(test_xir_root_conditional_${_root_conditional_gate}_compiler PROPERTIES
            LABELS "unit;xir;root-effects;higher-order;compiler-fault;ownership" TIMEOUT 600 RUN_SERIAL TRUE PROCESSORS 8 COST ${_root_source_compiler_cost})
    else()
        add_test(NAME test_xir_root_conditional_${_root_conditional_gate}_compiler
            COMMAND test_xir_root_conditional_${_root_conditional_gate} --compiler)
        set_tests_properties(test_xir_root_conditional_${_root_conditional_gate}_compiler PROPERTIES
            LABELS "unit;xir;root-effects;higher-order;compiler-fault;ownership" TIMEOUT 600 RUN_SERIAL TRUE PROCESSORS 1)
    endif()
endforeach()

# These literals remain reachable before the complete helper cost gate.
add_test(NAME test_xir_root_conditional_class_rows
    COMMAND test_xir_root_conditional_helpers --class-rows)
set_tests_properties(test_xir_root_conditional_class_rows PROPERTIES
    LABELS "unit;xir;root-effects;class;compiler;ownership" TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)

# Complete incoming actuals are exercised independently of the full cost gate.
add_test(NAME test_xir_root_conditional_parameter_coverage
    COMMAND test_xir_root_conditional_helpers --parameter-coverage)
set_tests_properties(test_xir_root_conditional_parameter_coverage PROPERTIES
    LABELS "unit;xir;root-effects;higher-order;compiler;ownership" TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)

# Existing producer identities must follow actual Source normalization.
add_executable(test_xir_source_effect_value_refine
    ${CMAKE_CURRENT_LIST_DIR}/test_xir_source_effect_value_refine.c)
target_include_directories(test_xir_source_effect_value_refine PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(test_xir_source_effect_value_refine PRIVATE xray_xir_source)
if(MSVC)
    target_compile_options(test_xir_source_effect_value_refine PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_source_effect_value_refine PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_source_effect_value_refine COMMAND test_xir_source_effect_value_refine)
set_tests_properties(test_xir_source_effect_value_refine PROPERTIES
    LABELS "unit;xir;root-effects;higher-order;compiler;ownership" TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)
add_test(NAME test_xir_source_effect_value_refine_compiler
    COMMAND test_xir_source_effect_value_refine --compiler)
set_tests_properties(test_xir_source_effect_value_refine_compiler PROPERTIES
    LABELS "unit;xir;root-effects;higher-order;compiler-fault;ownership" TIMEOUT 600 RUN_SERIAL TRUE PROCESSORS 1)

# A genuine open Fn(A)->B is checked in its own complete generic definition.
add_executable(test_xir_source_generic_callable_context
    ${CMAKE_CURRENT_LIST_DIR}/test_xir_source_generic_callable_context.c)
target_include_directories(test_xir_source_generic_callable_context PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(test_xir_source_generic_callable_context PRIVATE xray_xir_source)
if(MSVC)
    target_compile_options(test_xir_source_generic_callable_context PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_source_generic_callable_context PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_source_generic_callable_context COMMAND test_xir_source_generic_callable_context)
set_tests_properties(test_xir_source_generic_callable_context PROPERTIES
    LABELS "unit;xir;root-effects;higher-order;compiler;ownership" TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)
set(XRAY_GENERIC_CALLABLE_COMPILER_REPORTS_ROOT "" CACHE PATH
    "Exclusive qualification reports root outside Source and Build; empty uses system temporary storage")
set(_generic_compiler_report_arguments)
if(NOT XRAY_GENERIC_CALLABLE_COMPILER_REPORTS_ROOT STREQUAL "")
    list(APPEND _generic_compiler_report_arguments
        --reports-root "${XRAY_GENERIC_CALLABLE_COMPILER_REPORTS_ROOT}")
endif()
add_test(NAME test_xir_source_generic_callable_context_compiler
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/unit/xir/run_generic_callable_compiler_faults.py"
        --executable "$<TARGET_FILE:test_xir_source_generic_callable_context>"
        --source-root "${PROJECT_SOURCE_DIR}" --build-root "${CMAKE_BINARY_DIR}"
        ${_generic_compiler_report_arguments})
set_tests_properties(test_xir_source_generic_callable_context_compiler PROPERTIES
    LABELS "unit;xir;root-effects;higher-order;compiler-fault;ownership" TIMEOUT 600 RUN_SERIAL TRUE PROCESSORS 8 COST 600)

# The unique provenance receiver has its own whole-operation fault census.
add_test(NAME test_xir_source_provenance_take_resources
    COMMAND test_xir_source_generic_callable_context --provenance-take)
set_tests_properties(test_xir_source_provenance_take_resources PROPERTIES
    LABELS "unit;xir;root-effects;compiler-fault;ownership" TIMEOUT 600 RUN_SERIAL TRUE PROCESSORS 1)

# Exact instance indexing has a genuine Source scale and whole-operation census.
add_test(NAME test_xir_specialize_instance_index_scale
    COMMAND test_xir_source_generic_callable_context --spec-index)
set_tests_properties(test_xir_specialize_instance_index_scale PROPERTIES
    LABELS "unit;xir;specialization;performance;ownership" TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)
add_test(NAME test_xir_specialize_instance_index_resources
    COMMAND test_xir_source_generic_callable_context --spec-index-resources)
set_tests_properties(test_xir_specialize_instance_index_resources PROPERTIES
    LABELS "unit;xir;specialization;compiler-fault;ownership" TIMEOUT 600 RUN_SERIAL TRUE PROCESSORS 1)

# True Class subjects reach the same Cell terminal and retained-owner replay.
add_test(NAME test_xir_root_conditional_class_cells
    COMMAND test_xir_root_conditional_helpers --class-cells)
set_tests_properties(test_xir_root_conditional_class_cells PROPERTIES
    LABELS "unit;xir;root-effects;class;compiler;ownership" TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)
add_test(NAME test_xir_root_conditional_class_cells_compiler
    COMMAND test_xir_root_conditional_helpers --class-cells-compiler)
set_tests_properties(test_xir_root_conditional_class_cells_compiler PROPERTIES
    LABELS "unit;xir;root-effects;class;compiler-fault;ownership" TIMEOUT 600 RUN_SERIAL TRUE PROCESSORS 1)

add_test(NAME test_xir_root_conditional_ordinary_material
    COMMAND test_xir_root_conditional_helpers --ordinary-material)
set_tests_properties(test_xir_root_conditional_ordinary_material PROPERTIES
    TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)

# Initial certificates and transformed ordinary views have independent owners.
add_test(NAME test_xir_root_conditional_initial_ordinary_material
    COMMAND test_xir_root_conditional_helpers --initial-ordinary-material)
set_tests_properties(test_xir_root_conditional_initial_ordinary_material PROPERTIES
    LABELS "unit;xir;root-effects;compiler-fault;ownership" TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)

# Owned Error replay has an independent complete all-ordinal resource census.
add_test(NAME test_xir_root_conditional_owned_error_material
    COMMAND test_xir_root_conditional_helpers --owned-error-material)
set_tests_properties(test_xir_root_conditional_owned_error_material PROPERTIES
    LABELS "unit;xir;root-effects;compiler-fault;ownership" TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)

# Lossless codec controls and all actual faults precede unrelated cost gates.
add_test(NAME test_xir_root_conditional_compact_codec_material
    COMMAND test_xir_root_conditional_helpers --compact-codec-material)
set_tests_properties(test_xir_root_conditional_compact_codec_material PROPERTIES
    LABELS "unit;xir;root-effects;compiler-fault;ownership" TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)

# Owned canonical ranges retain complete provider replay and real fault census.
add_test(NAME test_xir_root_conditional_cell_owned_ranges
    COMMAND test_xir_root_conditional_helpers --cell-owned-ranges)
set_tests_properties(test_xir_root_conditional_cell_owned_ranges PROPERTIES
    LABELS "unit;xir;root-effects;compiler-fault;ownership" TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)

# Complete original generation faults precede unrelated cumulative cost gates.
add_test(NAME test_xir_root_conditional_generation_ranges
    COMMAND test_xir_root_conditional_helpers --generation-ranges)
set_tests_properties(test_xir_root_conditional_generation_ranges PROPERTIES
    LABELS "unit;xir;root-effects;compiler-fault;ownership" TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)

# Exact edge identity and every constant-row handler retain their own census.
add_test(NAME test_xir_root_conditional_invocation_edge_keys
    COMMAND test_xir_root_conditional_helpers --invocation-edge-keys)
set_tests_properties(test_xir_root_conditional_invocation_edge_keys PROPERTIES
    LABELS "unit;xir;root-effects;compiler-fault;ownership" TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)

# The original eight-stage pipeline has a separate one-operation measurement.
add_test(NAME test_xir_source_generic_callable_context_measure_original
    COMMAND test_xir_source_generic_callable_context --measure-original)
set_tests_properties(test_xir_source_generic_callable_context_measure_original PROPERTIES
    LABELS "unit;xir;root-effects;higher-order;compiler;ownership;scale-measurement;partial-feedback"
    TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)

# Native fixed/certified facts retain all three original storage recipes.
add_test(NAME test_xir_root_conditional_canonical_run_storage
    COMMAND test_xir_root_conditional_helpers --canonical-run-storage)
set_tests_properties(test_xir_root_conditional_canonical_run_storage PROPERTIES
    LABELS "unit;xir;root-effects;compiler-fault;ownership" TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)

add_test(NAME test_xir_effect_literal_body_storage
    COMMAND test_xir_root_conditional_helpers --literal-body)
set_tests_properties(test_xir_effect_literal_body_storage PROPERTIES
    LABELS "unit;xir;compiler;ownership" TIMEOUT 60 RUN_SERIAL TRUE PROCESSORS 1)
