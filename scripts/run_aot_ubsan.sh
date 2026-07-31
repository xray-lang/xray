#!/usr/bin/env bash
#
# run_aot_ubsan.sh - Run the GENERATED C under UndefinedBehaviorSanitizer.
#
# WHY THIS EXISTS
#
# Xray lowers to C and hands the result to clang/zig, so every UB rule of C
# applies to code no human wrote. The existing sanitizer lane
# (scripts/run_asan_focused.sh) instruments *the compiler* and compiles two real
# workloads with `--native --c-only` — it never compiles or runs the emitted C
# under a sanitizer. Nothing checked the output side.
#
# This lane closes that gap. It compiles each case with UBSan enabled on the
# generated translation unit, runs it, and requires:
#   - a clean exit,
#   - no sanitizer diagnostic,
#   - stdout identical to the VM's, so a sanitizer build that silently changes
#     behaviour is caught too.
#
# `-fno-sanitize-recover=all` makes the first finding fatal, so a diagnostic can
# never scroll past unnoticed.
#
# WHAT IS DELIBERATELY NOT ENABLED
#
# `unsigned-integer-overflow` and `unsigned-shift-base` are off: the code
# generator implements Xray's defined wrapping arithmetic by routing signed ops
# through uint64_t (`(int64_t)((uint64_t)a + (uint64_t)b)`), so unsigned wrap is
# the intended behaviour, not a defect. Everything else UBSan offers stays on —
# including signed overflow, shift bounds, alignment, null dereference, bad
# casts, and object size.
#
# The flags reach the C compiler through the project's own typed link plan
# (`xray.toml` [target.<triple>] cc_flags / ld_flags), so this lane exercises
# that plan rather than bypassing it.
#
# Environment overrides:
#   XRAY_BIN                 xray binary (default: build/xray)
#   XR_AOT_UBSAN_CASES       newline-separated .xr paths (default: the list below)
#   XR_AOT_UBSAN_TARGET      AOT target triple (default: probed from the host)
#   XR_AOT_UBSAN_TOOLCHAIN   AOT provider (default: clang)
#   XRAY_TOOLCHAIN_PROBE_SCALE  probe budget multiplier for loaded hosts
#
# Skips cleanly (exit 0) when the AOT toolchain is not READY, so hosts without a
# usable native provider do not report a false failure.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
XRAY_BIN="${XRAY_BIN:-${REPO_ROOT}/build/xray}"
TOOLCHAIN="${XR_AOT_UBSAN_TOOLCHAIN:-clang}"

# A loaded machine makes the readiness probe fail for reasons unrelated to the
# toolchain; give it room unless the caller already chose a value.
export XRAY_TOOLCHAIN_PROBE_SCALE="${XRAY_TOOLCHAIN_PROBE_SCALE:-12}"
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1:${UBSAN_OPTIONS:-}"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[0;33m'; NC='\033[0m'

echo "AOT UBSan lane — running generated C under UndefinedBehaviorSanitizer"
echo "========================================================================"

if [ ! -x "${XRAY_BIN}" ]; then
    echo "Error: xray not found at ${XRAY_BIN}"
    exit 1
fi

# ---------------------------------------------------------------------------
# Toolchain readiness. Not being able to build native code is a host property,
# not a defect in the code under test, so skip rather than fail.
# ---------------------------------------------------------------------------
DOCTOR_OUT="$("${XRAY_BIN}" toolchain doctor 2>&1)"
if ! printf '%s\n' "${DOCTOR_OUT}" | grep -q "AOT toolchain: READY"; then
    printf "${YELLOW}SKIP${NC}: AOT toolchain is not READY on this host\n"
    printf '%s\n' "${DOCTOR_OUT}" | sed 's/^/    /' | head -20
    exit 0
fi

TARGET="${XR_AOT_UBSAN_TARGET:-$(printf '%s\n' "${DOCTOR_OUT}" \
    | awk '/^  Target:/ { print $2; exit }')}"
if [ -z "${TARGET}" ]; then
    printf "${YELLOW}SKIP${NC}: could not determine the AOT target triple\n"
    exit 0
fi
echo "Target:     ${TARGET}"
echo "Toolchain:  ${TOOLCHAIN}"

# ---------------------------------------------------------------------------
# Cases. Chosen for UB density rather than breadth: integer wrap/convert paths,
# operator lowering, bit manipulation, indexing and slicing, string and byte
# buffers, and the struct/FFI layout surface. Each must be a self-contained
# single file that the VM can also run, so its stdout is a usable oracle.
# ---------------------------------------------------------------------------
default_cases() {
    cat <<'CASES'
tests/regression/03_operators/0300_arithmetic.xr
tests/regression/03_operators/0301_int64_overflow.xr
tests/regression/03_operators/0302_int64_native.xr
tests/regression/03_operators/0302_uint64_print_compare.xr
tests/regression/03_operators/0304_fixed_width_wrapping.xr
tests/regression/03_operators/0310_comparison.xr
tests/regression/03_operators/0330_bitwise.xr
tests/regression/03_operators/0331_shift_mod64.xr
tests/regression/03_operators/0332_int_bits_methods.xr
tests/regression/03_operators/0340_compound_assign.xr
tests/regression/03_operators/0006_as_numeric_cast.xr
tests/regression/01_literals/0101_int_boundary.xr
tests/regression/01_literals/0140_special_values.xr
tests/regression/01_literals/0120_string_basic.xr
tests/regression/06_collections/0600_array.xr
tests/regression/06_collections/0650_array_index_set_strict.xr
CASES
}

if [ -n "${XR_AOT_UBSAN_CASES:-}" ]; then
    CASES="${XR_AOT_UBSAN_CASES}"
else
    CASES="$(default_cases)"
fi

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/xray-aot-ubsan.XXXXXX")"
trap 'rm -rf "${WORK_DIR}"' EXIT

# The typed link plan is how a project declares extra C flags; using it here
# means this lane also proves that path keeps working.
cat > "${WORK_DIR}/xray.toml" <<EOF
[project]
name = "aot-ubsan-lane"
main = "case.xr"

[target.${TARGET}]
toolchain = "${TOOLCHAIN}"
cc_flags = [
    "-fsanitize=undefined,integer",
    "-fno-sanitize=unsigned-integer-overflow,unsigned-shift-base",
    "-fno-sanitize-recover=all",
    "-fno-omit-frame-pointer",
]
ld_flags = ["-fsanitize=undefined"]
EOF

PASSED=0; FAILED=0; SKIPPED=0; TOTAL=0
INSTRUMENTATION_VERIFIED=0

while IFS= read -r case_path; do
    [ -n "${case_path}" ] || continue
    abs_case="${REPO_ROOT}/${case_path}"
    if [ ! -f "${abs_case}" ]; then
        printf "  ${YELLOW}skip${NC} %-56s (missing)\n" "${case_path}"
        SKIPPED=$((SKIPPED + 1))
        continue
    fi
    TOTAL=$((TOTAL + 1))

    cp "${abs_case}" "${WORK_DIR}/case.xr"
    rm -f "${WORK_DIR}/case.bin"

    # VM oracle first: a case the VM cannot run has no expected output, so it
    # cannot distinguish "UBSan changed behaviour" from "case needs a fixture".
    vm_out="$(cd "${WORK_DIR}" && "${XRAY_BIN}" run case.xr 2>/dev/null)"
    if [ $? -ne 0 ]; then
        printf "  ${YELLOW}skip${NC} %-56s (VM run failed)\n" "${case_path}"
        SKIPPED=$((SKIPPED + 1)); TOTAL=$((TOTAL - 1))
        continue
    fi

    build_log="$(cd "${WORK_DIR}" && "${XRAY_BIN}" build -N \
        --target "${TARGET}" --toolchain "${TOOLCHAIN}" \
        -o case.bin case.xr 2>&1)"
    if [ ! -x "${WORK_DIR}/case.bin" ]; then
        printf "  ${RED}FAIL${NC} %-56s (AOT build failed)\n" "${case_path}"
        printf '%s\n' "${build_log}" | tail -12 | sed 's/^/        /'
        FAILED=$((FAILED + 1))
        continue
    fi

    # Self-check: a lane that silently stopped instrumenting would pass every
    # case forever. Require the sanitizer's check handlers to actually be linked
    # into the produced binary before trusting a clean run.
    if [ "${INSTRUMENTATION_VERIFIED}" -eq 0 ] && command -v nm >/dev/null 2>&1; then
        handlers="$(nm -u "${WORK_DIR}/case.bin" 2>/dev/null | grep -c "ubsan_handle")"
        if [ "${handlers}" -lt 1 ]; then
            printf "  ${RED}FAIL${NC} %-56s (no UBSan handlers linked)\n" "${case_path}"
            echo "        The generated binary carries no __ubsan_handle_* symbols, so this"
            echo "        lane would report success without checking anything. Verify that"
            echo "        xray.toml cc_flags still reach the C compile step."
            FAILED=$((FAILED + 1))
            continue
        fi
        echo "  (instrumentation verified: ${handlers} UBSan check handlers linked)"
        INSTRUMENTATION_VERIFIED=1
    fi

    run_out="$(cd "${WORK_DIR}" && ./case.bin 2>"${WORK_DIR}/stderr.txt")"
    run_rc=$?
    run_err="$(cat "${WORK_DIR}/stderr.txt")"

    if [ ${run_rc} -ne 0 ]; then
        printf "  ${RED}FAIL${NC} %-56s (exit ${run_rc})\n" "${case_path}"
        printf '%s\n' "${run_err}" | head -20 | sed 's/^/        /'
        FAILED=$((FAILED + 1))
        continue
    fi
    # halt_on_error makes a finding fatal, so a non-zero exit is the usual
    # signal; this catches a diagnostic that somehow did not abort.
    if printf '%s' "${run_err}" | grep -q "runtime error:"; then
        printf "  ${RED}FAIL${NC} %-56s (UBSan diagnostic)\n" "${case_path}"
        printf '%s\n' "${run_err}" | head -20 | sed 's/^/        /'
        FAILED=$((FAILED + 1))
        continue
    fi
    if [ "${run_out}" != "${vm_out}" ]; then
        printf "  ${RED}FAIL${NC} %-56s (stdout differs from VM)\n" "${case_path}"
        diff <(printf '%s\n' "${vm_out}") <(printf '%s\n' "${run_out}") \
            | head -12 | sed 's/^/        /'
        FAILED=$((FAILED + 1))
        continue
    fi

    printf "  ${GREEN}ok${NC}   %-56s\n" "${case_path}"
    PASSED=$((PASSED + 1))
done <<< "${CASES}"

echo "========================================================================"
echo "AOT UBSan: ${PASSED}/${TOTAL} passed, ${FAILED} failed, ${SKIPPED} skipped"

if [ ${TOTAL} -eq 0 ]; then
    printf "${YELLOW}SKIP${NC}: no runnable cases\n"
    exit 0
fi
if [ ${INSTRUMENTATION_VERIFIED} -eq 0 ] && command -v nm >/dev/null 2>&1; then
    printf "${RED}FAIL${NC}: never confirmed the sanitizer was linked\n"
    exit 1
fi
[ ${FAILED} -eq 0 ] || exit 1
exit 0
