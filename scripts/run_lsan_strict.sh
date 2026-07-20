#!/usr/bin/env bash
#
# run_lsan_strict.sh - Task 218 defense line 4: strict LeakSanitizer lane.
#
# Builds the compiler with ASan (+UBSan) and leak detection ON, then runs the
# focused unit-test surface with the committed suppression file. Process-wide
# registries are released via xr_process_shutdown() so a clean exit is a real
# signal rather than a suppressed one.
#
# PLATFORM: LeakSanitizer (standalone leak detection) is only supported under
# ASan on Linux. Apple/Homebrew clang on macOS/Darwin ships ASan WITHOUT a leak
# detector (this is exactly why the asan_focused lane runs detect_leaks=0). On
# non-Linux hosts this lane skips loudly rather than pretending to be green.
#
# Environment overrides:
#   XR_LSAN_JOBS         parallel build/test jobs (default: 8)
#   XR_LSAN_BUILD_DIR    build directory (default: build-lsan)
#   XR_LSAN_CTEST_REGEX  unit test name regex (default: ^test_)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${ROOT}"

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "== [lsan_strict] LeakSanitizer needs Linux (host is $(uname -s)); skipping."
    echo "== [lsan_strict] Run this lane on Linux CI to enforce the leak budget."
    exit 0
fi

JOBS="${XR_LSAN_JOBS:-8}"
BUILD="${XR_LSAN_BUILD_DIR:-build-lsan}"
CTEST_REGEX="${XR_LSAN_CTEST_REGEX:-^test_}"
SUPP="${ROOT}/scripts/lsan.supp"

export ASAN_OPTIONS="detect_leaks=1:abort_on_error=1:symbolize=1:detect_stack_use_after_return=1"
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"
export LSAN_OPTIONS="suppressions=${SUPP}:print_suppressions=1:report_objects=1"

echo "== [lsan_strict] configuring ${BUILD} (ASan+UBSan, detect_leaks=1)"
cmake -S . -B "${BUILD}" -G "Unix Makefiles" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DENABLE_ASAN=ON -DENABLE_UBSAN=ON \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

echo "== [lsan_strict] building (jobs=${JOBS})"
cmake --build "${BUILD}" -j"${JOBS}"

echo "== [lsan_strict] running unit tests under LSan (regex: ${CTEST_REGEX})"
ctest --test-dir "${BUILD}" --output-on-failure -j"${JOBS}" -R "${CTEST_REGEX}" --timeout 300

echo "== [lsan_strict] PASS"
