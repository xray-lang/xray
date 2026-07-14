#!/usr/bin/env bash
# Task 204 audit for byte receiver effects against the 203 storage model.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

: "${XRAY_BIN:?XRAY_BIN must point to the xray executable}"

TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/xray-byte-effect.XXXXXX")"
trap 'rm -rf "${TMP_DIR}"' EXIT

run_step() {
    local label="$1"
    shift
    printf '[byte-receiver-effect] %s\n' "${label}"
    "$@"
}

strip_ansi_file() {
    sed -E $'s/\x1B\\[[0-9;]*[a-zA-Z]//g' "$1"
}

expect_compile_error() {
    local rel="$1"
    local test_file="${PROJECT_ROOT}/${rel}"
    local expected_file="${test_file}.expected"
    local output_file="${TMP_DIR}/$(basename "${test_file}").out"
    local category_dir
    category_dir="$(dirname "${test_file}")"

    if [ ! -f "${test_file}" ]; then
        echo "missing compile-error case: ${rel}" >&2
        return 1
    fi
    if [ ! -f "${expected_file}" ]; then
        echo "missing expected diagnostics for: ${rel}" >&2
        return 1
    fi

    local typepath="${category_dir}"
    if [ -n "${XRAY_TYPEPATH:-}" ]; then
        typepath="${typepath}:${XRAY_TYPEPATH}"
    fi

    if XRAY_TYPEPATH="${typepath}" "${XRAY_BIN}" "${test_file}" >"${output_file}" 2>&1; then
        echo "expected compile error but program compiled: ${rel}" >&2
        return 1
    fi

    local output
    output="$(strip_ansi_file "${output_file}")"

    local expected
    while IFS= read -r expected; do
        [ -n "${expected}" ] || continue
        if ! printf '%s' "${output}" | grep -Fq "${expected}"; then
            echo "diagnostic mismatch for ${rel}" >&2
            echo "missing: ${expected}" >&2
            echo "output:" >&2
            printf '%s\n' "${output}" >&2
            return 1
        fi
    done <"${expected_file}"
}

POSITIVE_TESTS=(
    "tests/regression/14_typed_array/1416_byte_receiver_effect_matrix.xr"
    "tests/regression/14_typed_array/1410_shared_provenance_rebind_reset.xr"
    "tests/regression/14_typed_array/1411_shared_provenance_readonly_param.xr"
    "tests/regression/14_typed_array/1412_shared_provenance_readonly_function_value_param.xr"
    "tests/regression/14_typed_array/1413_shared_provenance_imported_readonly_function_value_param.xr"
    "tests/regression/14_typed_array/1414_shared_provenance_reexported_readonly_function_value_param.xr"
    "tests/regression/14_typed_array/1415_shared_provenance_returned_readonly_function_value_param.xr"
    "tests/regression/09_advanced/0916_owned_binding.xr"
)

NEGATIVE_TESTS=(
    "tests/compile_errors/type/byte_array_append_from_rejects_in.xr"
    "tests/compile_errors/type/byte_slice_repeat_from_rejects_in.xr"
    "tests/compile_errors/type/const_byte_slice_mutating_method.xr"
    "tests/compile_errors/type/const_byte_slice_index_store.xr"
    "tests/compile_errors/type/shared_byte_array_mutating_method.xr"
    "tests/compile_errors/type/103_shared_derived_slice_index_store_rejected.xr"
    "tests/compile_errors/type/104_shared_derived_slice_mutating_method_rejected.xr"
    "tests/compile_errors/type/107_shared_derived_slice_mutating_param_rejected.xr"
    "tests/compile_errors/type/108_shared_derived_slice_transitive_mutating_param_rejected.xr"
    "tests/compile_errors/type/110_shared_derived_function_value_mutating_param_rejected.xr"
    "tests/compile_errors/type/111_shared_derived_dynamic_function_value_mutating_param_rejected.xr"
    "tests/compile_errors/type/112_shared_derived_unknown_function_value_param_rejected.xr"
    "tests/compile_errors/type/113_shared_derived_imported_function_value_mutating_param_rejected.xr"
    "tests/compile_errors/type/114_shared_derived_namespace_function_value_mutating_param_rejected.xr"
    "tests/compile_errors/type/115_shared_derived_reexported_function_value_mutating_param_rejected.xr"
    "tests/compile_errors/type/116_shared_derived_star_reexported_function_value_mutating_param_rejected.xr"
    "tests/compile_errors/type/117_shared_derived_returned_function_value_mutating_param_rejected.xr"
    "tests/compile_errors/ownership/078_move_owner_active_span_borrow_rejected.xr"
    "tests/compile_errors/ownership/079_freeze_owner_active_span_borrow_rejected.xr"
)

for rel in "${POSITIVE_TESTS[@]}"; do
    run_step "positive ${rel}" "${XRAY_BIN}" test "${PROJECT_ROOT}/${rel}"
done

for rel in "${NEGATIVE_TESTS[@]}"; do
    run_step "negative ${rel}" expect_compile_error "${rel}"
done
