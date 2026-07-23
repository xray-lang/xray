#!/usr/bin/env bash
# Task 239 final audit for byte/u8 canonical U8 identity.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

: "${XRAY_BIN:?XRAY_BIN must point to the xray executable}"
: "${XRAY_TEST_LSP_DOCUMENT:?XRAY_TEST_LSP_DOCUMENT must point to test_lsp_document}"
: "${XRAY_TEST_XGLOBAL_SUMMARY:?XRAY_TEST_XGLOBAL_SUMMARY must point to test_xglobal_summary}"

REGRESSION_CASE="${PROJECT_ROOT}/tests/regression/14_typed_array/1409_byte_u8_canonical_identity.xr"

run_step() {
    local label="$1"
    shift
    printf '[byte-u8-canonical] %s\n' "${label}"
    "$@"
}

run_step "language identity regression" "${XRAY_BIN}" test "${REGRESSION_CASE}"
run_step "LSP canonical byte display/docs" "${XRAY_TEST_LSP_DOCUMENT}"
run_step "global evidence/cache canonical U8 type keys" "${XRAY_TEST_XGLOBAL_SUMMARY}"
