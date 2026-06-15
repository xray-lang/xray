#!/usr/bin/env bash
# ============================================================================
# check_xi_aot_generated_sync.sh
# ----------------------------------------------------------------------------
# Ensures that Xi semantic and AOT target-layer generated artifacts match their
# xisa source descriptions. This is shared by Xi and codegen invariant gates so
# the semantic-source freshness contract has one implementation.
# ============================================================================

set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"

FAIL=0
TMP_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/xray_xi_aot_sync.XXXXXX")
GEN_LOG=$(mktemp "${TMPDIR:-/tmp}/xray_xi_aot_sync_log.XXXXXX")
trap 'rm -rf "${TMP_ROOT}"; rm -f "${GEN_LOG}"' EXIT

red() { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
section() { printf '\n=== %s ===\n' "$*"; }

require_file() {
    local path="$1"
    if [ ! -f "${path}" ]; then
        red "FAIL: required xisa source is missing: ${path}"
        FAIL=1
        return 1
    fi
    return 0
}

run_gen() {
    if ! "$@" >"${GEN_LOG}" 2>&1; then
        red "FAIL: generator command failed:"
        printf '  %s\n' "$*"
        sed 's/^/  /' "${GEN_LOG}"
        FAIL=1
        return 1
    fi
    return 0
}

compare_artifacts() {
    local group="$1"
    shift
    local group_fail=0

    for rel in "$@"; do
        if ! cmp -s "${TMP_ROOT}/${rel}" "${rel}"; then
            red "FAIL: generated ${group} artifact is stale: ${rel}"
            group_fail=1
            FAIL=1
        fi
    done

    if [ "${group_fail}" -eq 0 ]; then
        green "OK: generated ${group} artifacts match xisa sources."
    fi
}

section "Xi semantic generated artifacts"
if require_file tools/xisagen/xisagen.py &&
   require_file xisa/xi/ops.def &&
   require_file xisa/xi/lowering.def &&
   require_file xisa/xi/verifier.def; then
    if run_gen python3 tools/xisagen/xisagen.py xi-ops \
            xisa/xi/ops.def "${TMP_ROOT}/src/ir/xi_ops_gen.h" &&
       run_gen python3 tools/xisagen/xisagen.py xi-verify \
            xisa/xi/ops.def xisa/xi/verifier.def "${TMP_ROOT}/src/ir/xi_verify_gen.h" &&
       run_gen python3 tools/xisagen/xisagen.py xi-lowering \
            xisa/xi/ops.def xisa/xi/lowering.def "${TMP_ROOT}"; then
        compare_artifacts "Xi" \
            src/ir/xi_ops_gen.h \
            src/ir/xi_verify_gen.h \
            src/ir/xi_lowering_coverage_gen.h \
            src/ir/xi_emit_vm_gen.h \
            src/vm/xvm_template_width_gen.inc.c \
            src/vm/xvm_template_bitwise_binary_gen.inc.c \
            src/vm/xvm_template_bitwise_unary_gen.inc.c \
            src/vm/xvm_template_shift_gen.inc.c \
            src/vm/xvm_template_compare_gen.inc.c \
            src/jit/xi_to_xm_dispatch_gen.h \
            src/aot/xi_to_c_dispatch_gen.h \
            src/aot/xi_to_c_stmt_dispatch_gen.h \
            tests/unit/ir/test_xi_lowering_gen.c
    fi
fi

section "AOT target generated artifacts"
if require_file tools/xisagen/xisagen.py &&
   require_file xisa/aot/rep.def &&
   require_file xisa/aot/abi.def &&
   require_file xisa/aot/layout.def; then
    if run_gen python3 tools/xisagen/xisagen.py aot-rep \
            xisa/aot/rep.def "${TMP_ROOT}/src/aot/xaot_rep_gen.h" &&
       run_gen python3 tools/xisagen/xisagen.py aot-abi \
            xisa/aot/rep.def xisa/aot/abi.def "${TMP_ROOT}/src/aot/xaot_abi_gen.h" &&
       run_gen python3 tools/xisagen/xisagen.py aot-layout \
            xisa/aot/rep.def xisa/aot/layout.def "${TMP_ROOT}/src/aot/xaot_layout_gen.h"; then
        compare_artifacts "AOT" \
            src/aot/xaot_rep_gen.h \
            src/aot/xaot_abi_gen.h \
            src/aot/xaot_layout_gen.h
    fi
fi

exit "${FAIL}"
