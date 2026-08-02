#!/usr/bin/env bash
#
# run_tsan_focused.sh - Task 260 §8 F.2: focused ThreadSanitizer lane.
#
# The work-stealing scheduler and the two-band reference counter are exactly
# the code TSan exists for, and until this lane nothing ran them under it.
# The three legacy directories the 2026-08-01 analysis pointed at
# (tests/coroutine_safety, tests/work_stealing, tests/network) are NOT the
# vehicle: their own READMEs mark them as historical hand-run drafts on
# pre-clean-slate surface, superseded by tests/regression/11_coroutine and
# the diff nets. This lane therefore runs the CURRENT canonical concurrency
# assets, VM-mode (the TSan-instrumented binary is the compiler+runtime
# itself; AOT child binaries are produced by the system cc and would not be
# instrumented).
#
# Scope: a curated channel/work-stealing/scope subset of the coroutine
# regression suite plus the multi-worker liveness shapes, each run under the
# TSan build with races collected, not halted on. The lane fails when any
# ThreadSanitizer warning appears.
#
# Environment overrides:
#   XR_TSAN_JOBS        parallel build jobs (default: all cores)
#   XR_TSAN_BUILD_DIR   TSan build directory (default: build-tsan)
#   XR_TSAN_SKIP_BUILD  1 = reuse the existing TSan build as-is

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${ROOT}"

if command -v nproc >/dev/null 2>&1; then
    DEFAULT_JOBS="$(nproc)"
elif command -v sysctl >/dev/null 2>&1; then
    DEFAULT_JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 8)"
else
    DEFAULT_JOBS=8
fi
JOBS="${XR_TSAN_JOBS:-${DEFAULT_JOBS}}"
TSAN_BUILD="${XR_TSAN_BUILD_DIR:-build-tsan}"

if [ "${XR_TSAN_SKIP_BUILD:-0}" != "1" ]; then
    if [ ! -f "${TSAN_BUILD}/CMakeCache.txt" ]; then
        cmake -B "${TSAN_BUILD}" -G Ninja \
            -DCMAKE_BUILD_TYPE=Debug \
            -DCMAKE_C_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
            -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
    fi
    cmake --build "${TSAN_BUILD}" --target xray -j "${JOBS}"
fi

XRAY="${TSAN_BUILD}/xray"
if [ ! -x "${XRAY}" ]; then
    echo "FAIL: TSan xray binary missing: ${XRAY}"
    exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/xray_tsan.XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

# collect, do not halt: one run surfaces every distinct race in the shape
export TSAN_OPTIONS="halt_on_error=0 exitcode=0 log_path=${WORK}/tsan"

# Channel / work-stealing / scope shapes from the canonical suite, plus the
# progress shapes the liveness lane pinned. Multi-worker forced where the
# default profile would stay single (races need real parallelism).
CASES=(
    "test tests/regression/11_coroutine/1101_channel_basic.xr"
    "test tests/regression/11_coroutine/1102_go_await.xr"
    "test tests/regression/11_coroutine/1104_coroutine_combined.xr"
    "test tests/regression/11_coroutine/1148_scope_race_stress.xr"
    "test tests/regression/11_coroutine/1151_inject_queue_spill.xr"
    "test tests/regression/11_coroutine/1167_channel_explicit_transfer.xr"
    "run tests/diff/cases/liveness/busy_poll_progress.xr"
    "run tests/diff/cases/liveness/blocking_recv_progress.xr"
    "run tests/diff/cases/liveness/cancel_responsive_blocking.xr"
)

echo "=== TSan focused lane ==="
echo "Binary: ${XRAY}"
FAILED=0
for entry in "${CASES[@]}"; do
    mode="${entry%% *}"
    file="${entry#* }"
    name="$(basename "${file}" .xr)"
    printf '  %-40s' "${name}"
    if XRAY_WORKERS=4 timeout 60 "${XRAY}" ${mode} "${file}" \
        >"${WORK}/${name}.out" 2>&1; then
        echo "ran"
    else
        echo "ran (nonzero exit $?)"
    fi
done

REPORTS="$(cat "${WORK}"/tsan.* 2>/dev/null | grep -c "WARNING: ThreadSanitizer" || true)"
if [ "${REPORTS:-0}" -gt 0 ]; then
    echo ""
    echo "ThreadSanitizer reported ${REPORTS} warning(s):"
    grep -A 3 "WARNING: ThreadSanitizer" "${WORK}"/tsan.* 2>/dev/null | head -60
    echo "VERDICT: FAIL (${REPORTS} TSan warnings)"
    exit 1
fi
echo ""
echo "VERDICT: PASS (no ThreadSanitizer warnings across ${#CASES[@]} cases)"
