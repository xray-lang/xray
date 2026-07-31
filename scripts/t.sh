#!/usr/bin/env bash
#
# t.sh - Tiered test runner.
#
# WHY TIERS, AND WHY THIS PARTICULAR CUT
#
# A full ctest run is ~30-40 minutes, but the cost is not spread across the 321
# tests — it is concentrated in a handful that shell out to a C compiler. From a
# measured full run:
#
#     aot_standalone_suite            342s
#     asan_focused                    331s   (rebuilds the whole compiler + ASan)
#     test_string_native_error_abi     93s
#     test_crypto_native_error_abi     63s
#     test_param_contract_aot          60s
#     test_compress_native_error_abi   32s
#     ---------------------------------------
#     the other ~170 unit tests       ~10s   combined
#
# `ctest -R "^test_"` takes 6m36s at 4% CPU: almost all of it is waiting on
# spawned clang processes, not computing. So the useful axis is not "how many
# tests" but "does this test invoke an external toolchain". That is the cut
# below, and it is why t0 can be seconds while still running every in-process
# test there is.
#
# Each tier is a superset of the one before it. Nothing is sampled or truncated
# inside a tier — a tier either runs a suite completely or does not claim it —
# and every run prints what it did not cover, so a green t0 is never mistaken
# for a green suite.
#
# USAGE
#     scripts/t.sh t0      after an edit            target < 30s
#     scripts/t.sh t1      before a commit          target < 3min
#     scripts/t.sh t2      before a push            target < 12min
#     scripts/t.sh t3      periodic / release       everything
#     scripts/t.sh auto    pick a tier from the working-tree diff
#
# Extra arguments are forwarded to ctest, e.g.
#     scripts/t.sh t1 --rerun-failed
#     scripts/t.sh t0 -R parser
#
# Environment:
#     XR_BUILD_DIR   build directory (default: build)
#     XR_JOBS        parallelism (default: cores - 2)
#     XR_NO_BUILD=1  skip the incremental build step

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "${REPO_ROOT}" || exit 1

BUILD_DIR="${XR_BUILD_DIR:-build}"
if command -v nproc >/dev/null 2>&1; then
    CORES="$(nproc)"
elif command -v sysctl >/dev/null 2>&1; then
    CORES="$(sysctl -n hw.ncpu)"
else
    CORES=4
fi
JOBS="${XR_JOBS:-$(( CORES > 3 ? CORES - 2 : 1 ))}"

RED=$'\033[0;31m'; GREEN=$'\033[0;32m'; YELLOW=$'\033[0;33m'
BLUE=$'\033[0;34m'; BOLD=$'\033[1m'; NC=$'\033[0m'

# Tests that spawn an external C toolchain. Excluded below t3 — not because
# they matter less, but because each one costs more than every in-process test
# combined. Keep this list ordered by the measured cost in the header.
SLOW_EXTERNAL='aot_standalone_suite|asan_focused|lsan_strict|aot_ubsan|test_string_native_error_abi|test_crypto_native_error_abi|test_param_contract_aot|test_compress_native_error_abi'

# QEMU-backed cross targets: slow, and unavailable on most developer machines.
SLOW_QEMU='aot_freestanding_qemu_smoke|aot_freestanding_riscv_qemu_smoke|aot_freestanding_thumb_qemu_smoke|aot_cross_smoke|aot_bundled_zig_smoke'

usage() {
    sed -n '/^# USAGE/,/^$/p' "$0" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

TIER="${1:-}"
[ -n "${TIER}" ] || usage 1
shift || true
case "${TIER}" in
    -h|--help|help) usage 0 ;;
esac

# ---------------------------------------------------------------------------
# auto: choose a tier from what the working tree actually touches.
#
# Deliberately picks a TIER rather than a hand-picked set of test names. Tier
# selection is a coverage floor that can be reasoned about; a bespoke per-change
# test list is where coverage goes missing without anyone noticing.
# ---------------------------------------------------------------------------
if [ "${TIER}" = "auto" ]; then
    changed="$( { git diff --name-only HEAD 2>/dev/null;
                  git ls-files --others --exclude-standard 2>/dev/null; } | sort -u )"
    if [ -z "${changed}" ]; then
        echo "auto: working tree is clean — running t0"
        TIER=t0
    elif printf '%s\n' "${changed}" | grep -qE '^src/(aot|ir|coro|vm|runtime)/|^CMakeLists\.txt$|^xisa/'; then
        TIER=t2
        echo "auto: backend / runtime / build changes — running ${TIER}"
    elif printf '%s\n' "${changed}" | grep -qE '^src/'; then
        TIER=t1
        echo "auto: compiler source changes — running ${TIER}"
    else
        TIER=t0
        echo "auto: no compiler source changes — running ${TIER}"
    fi
    printf '%s\n' "${changed}" | sed 's/^/       /' | head -12
    [ "$(printf '%s\n' "${changed}" | wc -l)" -gt 12 ] && echo "       ..."
fi

case "${TIER}" in
    t0|t1|t2|t3) ;;
    *) echo "Unknown tier '${TIER}'"; usage 1 ;;
esac

if [ ! -d "${BUILD_DIR}" ]; then
    echo "${RED}Error${NC}: build directory '${BUILD_DIR}' does not exist."
    echo "Configure one first, e.g.:  cmake -B ${BUILD_DIR} -DCMAKE_BUILD_TYPE=Debug"
    exit 1
fi

echo "${BOLD}tier ${TIER}${NC}  build=${BUILD_DIR}  jobs=${JOBS}"
echo "========================================================================"

START="$(date +%s)"

# ---------------------------------------------------------------------------
# Build. Every tier needs a current binary; without this a tier can "pass"
# against a stale one, which is worse than not running at all.
# ---------------------------------------------------------------------------
if [ "${XR_NO_BUILD:-0}" != "1" ]; then
    echo "${BLUE}==>${NC} building"
    build_targets=(xray)
    [ "${TIER}" = "t0" ] || build_targets=()   # t1+ needs the test binaries too
    if ! cmake --build "${BUILD_DIR}" -j"${JOBS}" ${build_targets[@]+"${build_targets[@]/#/--target }"} \
            >"${BUILD_DIR}/.t-build.log" 2>&1; then
        echo "${RED}BUILD FAILED${NC}"
        grep -E "error:" "${BUILD_DIR}/.t-build.log" | head -20
        exit 1
    fi
fi

# ---------------------------------------------------------------------------
# Tier composition. Expressed as ctest -R / -E so the tiers stay describable in
# one line each and cannot drift from what ctest actually selects.
# ---------------------------------------------------------------------------
case "${TIER}" in
    t0)
        # Everything that runs in-process: the unit tests plus the static gates
        # (residue / convergence / sync / harness / check_*). No toolchain spawn.
        INCLUDE='^(test_|.*_residue$|.*_convergence.*|.*_sync$|.*_inventory.*|harness_|check_|stdlib_boundary_|stdlib_def_|stdlib_metadata|contract_freeze|surface_drift)'
        EXCLUDE="${SLOW_EXTERNAL}|${SLOW_QEMU}"
        NOT_COVERED="VM/AOT differential, regression corpus, AOT suites, sanitizers"
        ;;
    t1)
        # + the VM-executed corpora: regression, syntax, bytecode, stdlib.
        INCLUDE=''
        EXCLUDE="${SLOW_EXTERNAL}|${SLOW_QEMU}|^backend_diff|^task190_|^aot_|^ffi_|^install_|^native_output|^binary_|^dap_|^raw_scalar|^global_evidence|^byte_array_aot"
        NOT_COVERED="VM/AOT differential, AOT suites, sanitizers, QEMU cross"
        ;;
    t2)
        # + differential and AOT, including the generated-C UBSan lane. This is
        # the tier that can actually catch a backend divergence.
        INCLUDE=''
        EXCLUDE="aot_standalone_suite|asan_focused|lsan_strict|${SLOW_QEMU}"
        NOT_COVERED="ASan/LSan lanes, aot_standalone_suite, QEMU cross targets"
        ;;
    t3)
        INCLUDE=''
        EXCLUDE=''
        NOT_COVERED=""
        ;;
esac

CTEST_ARGS=(--output-on-failure -j"${JOBS}")
[ -n "${INCLUDE}" ] && CTEST_ARGS+=(-R "${INCLUDE}")
[ -n "${EXCLUDE}" ] && CTEST_ARGS+=(-E "${EXCLUDE}")

SELECTED="$(cd "${BUILD_DIR}" && ctest -N "${CTEST_ARGS[@]}" "$@" 2>/dev/null | grep -c 'Test *#')"
TOTAL_TESTS="$(cd "${BUILD_DIR}" && ctest -N 2>/dev/null | grep -c 'Test *#')"
echo "${BLUE}==>${NC} ctest: ${SELECTED}/${TOTAL_TESTS} tests"

(cd "${BUILD_DIR}" && ctest "${CTEST_ARGS[@]}" "$@")
RC=$?

# ---------------------------------------------------------------------------
# The tests/regression corpus (457 cases) has no ctest entry and no CMakeLists
# reference — it only ever ran in one non-blocking nightly lane, and 48 cases
# had rotted unnoticed. Gate it here, against a ratchet baseline so the tier is
# usable from day one: any failure not in the baseline fails the tier, and a
# baseline entry that starts passing fails too, so the list can only shrink.
#
# t1 runs it VM-only (~1m10s). t2 lets it include its own backend-diff net.
# ---------------------------------------------------------------------------
run_regression_corpus() {
    local skip_diff="$1" baseline="tests/regression/baseline_failures.txt"
    echo "${BLUE}==>${NC} regression corpus (457 cases)$([ "${skip_diff}" = 1 ] && echo ", VM only")"

    local log; log="$(mktemp)"
    XRAY_SKIP_BACKEND_DIFF="${skip_diff}" XRAY_BIN="${REPO_ROOT}/${BUILD_DIR}/xray" \
        bash scripts/run_regression_tests.sh >"${log}" 2>&1
    grep -E "^总文件数|^通过|^失败" "${log}" | sed 's/^/    /'

    local actual expected
    actual="$(grep -A200 "失败的测试" "${log}" | grep "^  - " | sed "s/^  - //" | sort -u)"
    expected="$(grep -vE "^#|^$" "${baseline}" | sort -u)"

    local newly_broken newly_fixed
    newly_broken="$(comm -23 <(printf "%s\n" "${actual}") <(printf "%s\n" "${expected}") | grep -v "^$")"
    newly_fixed="$(comm -13 <(printf "%s\n" "${actual}") <(printf "%s\n" "${expected}") | grep -v "^$")"
    rm -f "${log}"

    local rc=0
    if [ -n "${newly_broken}" ]; then
        echo "${RED}    regression: newly broken${NC}"
        printf "%s\n" "${newly_broken}" | sed "s/^/      /"
        rc=1
    fi
    if [ -n "${newly_fixed}" ]; then
        echo "${YELLOW}    regression: now passing — delete these from ${baseline}${NC}"
        printf "%s\n" "${newly_fixed}" | sed "s/^/      /"
        rc=1
    fi
    [ ${rc} -eq 0 ] && echo "${GREEN}    regression: no change against baseline${NC}"
    return ${rc}
}

case "${TIER}" in
    t1)     run_regression_corpus 1 || RC=1 ;;
    t2|t3)  run_regression_corpus 0 || RC=1 ;;
esac

# ---------------------------------------------------------------------------
# t0 additionally runs the compile-error corpus. It is a shell script rather
# than a ctest entry in some configurations, and it is the fastest broad check
# of parser and analyzer diagnostics there is (732 cases).
# ---------------------------------------------------------------------------
if [ "${TIER}" = "t0" ] && [ ${RC} -eq 0 ]; then
    echo "${BLUE}==>${NC} compile-error corpus"
    if ! XRAY_BIN="${REPO_ROOT}/${BUILD_DIR}/xray" \
            bash tests/compile_errors/run_compile_error_tests.sh 2>&1 | tail -6; then
        RC=1
    fi
fi

ELAPSED=$(( $(date +%s) - START ))
echo "========================================================================"
printf "tier ${BOLD}%s${NC}  %s  %dm%02ds\n" "${TIER}" \
    "$([ ${RC} -eq 0 ] && printf "${GREEN}PASS${NC}" || printf "${RED}FAIL${NC}")" \
    $((ELAPSED / 60)) $((ELAPSED % 60))

# A tier that stayed quiet about its own limits is how a green check turns into
# false confidence. Say what was not covered, every time.
if [ -n "${NOT_COVERED}" ]; then
    echo "${YELLOW}not covered by ${TIER}${NC}: ${NOT_COVERED}"
    case "${TIER}" in
        t0) echo "  next: scripts/t.sh t1" ;;
        t1) echo "  next: scripts/t.sh t2" ;;
        t2) echo "  next: scripts/t.sh t3" ;;
    esac
fi

exit ${RC}
