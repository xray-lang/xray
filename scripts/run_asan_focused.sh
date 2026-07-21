#!/usr/bin/env bash
#
# run_asan_focused.sh - Task 218 defense line 2: focused ASan+UBSan lane.
#
# Builds the compiler with AddressSanitizer + UndefinedBehaviorSanitizer and
# runs a focused, time-bounded surface that reliably reproduces the compiler
# memory-safety accident classes from task 209:
#
#   1. all C unit tests
#   2. a fast backend-diff subset (VM vs native, task-190 cases)
#   3. two real workloads compiled end-to-end to C: the xxhash port and the
#      committed bili-analysis-server fixture.
#
# Leaks are intentionally OFF here (detect_leaks=0); process-exit leaks are the
# job of the separate lsan_strict lane (task 218 P4). ASan/UBSan stay fully on.
#
# The ASan build reuses the `asan-jit-debug` CMake preset (binaryDir build-asan)
# when a Ninja generator is available, and otherwise falls back to a direct
# configure with the same ENABLE_ASAN / ENABLE_UBSAN cache variables using the
# default Make generator, so the lane runs in environments without Ninja.
#
# Environment overrides:
#   XR_ASAN_JOBS          parallel build/test jobs (default: 8)
#   XR_ASAN_BUILD_DIR     ASan build directory (default: build-asan)
#   XR_ASAN_CTEST_REGEX   unit test name regex (default: ^test_)
#   XR_ASAN_DIFF_REGEX    backend-diff subset regex (default: task190_.*_backend_diff)
#   XR_ASAN_XXHASH_MAIN   path to the xxhash port entry (default: repo-relative sibling)
#   XR_ASAN_BILI_MAIN     path to the committed bili-analysis-server fixture
#   XR_ASAN_SKIP_BUILD    if set to 1, reuse an existing ASan build

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${ROOT}"

JOBS="${XR_ASAN_JOBS:-8}"
ASAN_BUILD="${XR_ASAN_BUILD_DIR:-build-asan}"
CTEST_REGEX="${XR_ASAN_CTEST_REGEX:-^test_}"
# Exclude native-toolchain / subprocess integration tests from the ASan
# *memory-safety* surface: they drive a full native AOT compile+link (assuming
# the default `build/` cache layout) or run the compiler as a subprocess under
# tight hardcoded timeouts, so they fail under ASan for environmental reasons
# (slowdown, cache mismatch) rather than any memory bug. The ordinary ctest run
# still covers VM/native ABI parity. The pure C unit tests remain in scope.
CTEST_EXCLUDE="${XR_ASAN_CTEST_EXCLUDE:-native_error_abi|param_mode_diagnostics|param_contract}"
# Fast backend-diff subset. The `layout`, `mem`, and `extern` task-190 case
# sets are ASan-clean. The `extern` (FFI) set's earlier heap-buffer-overflow was
# a test-case defect, not a compiler bug: extern_scalar_descriptor_matrix.xr
# passed a 3-byte, non-NUL-terminated byte literal to C strlen, an out-of-bounds
# read by construction (byte literals are exactly-N-byte arrays, not C strings).
# The case now NUL-terminates that input, so the FFI marshalling is well-formed
# and the whole task-190 diff surface runs under ASan.
DIFF_REGEX="${XR_ASAN_DIFF_REGEX:-task190_.*_backend_diff}"
XXHASH_MAIN="${XR_ASAN_XXHASH_MAIN:-${ROOT}/../../xray-ports/ports/xxhash/src/main.xr}"
BILI_MAIN="${XR_ASAN_BILI_MAIN:-${ROOT}/tests/meta/fixtures/bili-analysis-server/src/main.xr}"

# Sanitizer runtime config. Leaks are handled by the lsan_strict lane; keep
# ASan/UBSan themselves aborting on the first real error.
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=1:symbolize=1:strict_string_checks=1:detect_stack_use_after_return=1"
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"

echo "== [asan_focused] ROOT=${ROOT}"
echo "== [asan_focused] build dir=${ASAN_BUILD} jobs=${JOBS}"

# ---------------------------------------------------------------------------
# 1. Configure + build the ASan/UBSan compiler and tests.
# ---------------------------------------------------------------------------
if [[ "${XR_ASAN_SKIP_BUILD:-0}" != "1" ]]; then
    if command -v ninja >/dev/null 2>&1 && [[ -f CMakePresets.json ]]; then
        echo "== [asan_focused] configuring via CMake preset asan-jit-debug (Ninja)"
        cmake --preset asan-jit-debug
    else
        echo "== [asan_focused] Ninja unavailable; configuring build-asan directly (Make)"
        cmake -S . -B "${ASAN_BUILD}" -G "Unix Makefiles" \
            -DCMAKE_BUILD_TYPE=Debug \
            -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
            -DENABLE_ASAN=ON -DENABLE_UBSAN=ON \
            -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    fi
    echo "== [asan_focused] building compiler + tests (ASan/UBSan)"
    cmake --build "${ASAN_BUILD}" -j"${JOBS}"
fi

XRAY_BIN="${ROOT}/${ASAN_BUILD}/xray"
if [[ ! -x "${XRAY_BIN}" ]]; then
    echo "!! [asan_focused] ASan xray binary not found at ${XRAY_BIN}" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# 2. Unit tests under ASan/UBSan.
# ---------------------------------------------------------------------------
echo "== [asan_focused] running unit tests (regex: ${CTEST_REGEX}, exclude: ${CTEST_EXCLUDE})"
ctest --test-dir "${ASAN_BUILD}" --output-on-failure -j"${JOBS}" \
    -R "${CTEST_REGEX}" -E "${CTEST_EXCLUDE}" --timeout 300

# ---------------------------------------------------------------------------
# 3. Fast backend-diff subset under ASan/UBSan.
# ---------------------------------------------------------------------------
echo "== [asan_focused] running fast backend-diff subset (regex: ${DIFF_REGEX})"
if ctest --test-dir "${ASAN_BUILD}" -N -R "${DIFF_REGEX}" 2>/dev/null | grep -qE "Test +#"; then
    ctest --test-dir "${ASAN_BUILD}" --output-on-failure -j"${JOBS}" -R "${DIFF_REGEX}" --timeout 600
else
    echo "== [asan_focused] no backend-diff tests matched ${DIFF_REGEX}; skipping"
fi

# ---------------------------------------------------------------------------
# 4. Real workload: full AOT compile (emit C only) of the xxhash port.
# ---------------------------------------------------------------------------
if [[ -f "${XXHASH_MAIN}" ]]; then
    XXHASH_PROJECT="$(cd "$(dirname "${XXHASH_MAIN}")/.." && pwd)"
    OUT_C="$(mktemp -t asan_xxhash_XXXXXX).c"
    echo "== [asan_focused] AOT-compiling (emit C only) xxhash port: ${XXHASH_MAIN}"
    ( cd "${XXHASH_PROJECT}" && "${XRAY_BIN}" build "${XXHASH_MAIN}" --native --c-only -o "${OUT_C}" )
    echo "== [asan_focused] xxhash AOT emit OK: $(wc -c <"${OUT_C}") bytes -> ${OUT_C}"
    rm -f "${OUT_C}"
else
    echo "== [asan_focused] xxhash port not found at ${XXHASH_MAIN}; skipping real-workload compile"
fi

if [[ -f "${BILI_MAIN}" ]]; then
    BILI_PROJECT="$(cd "$(dirname "${BILI_MAIN}")/.." && pwd)"
    OUT_C="$(mktemp -t asan_bili_XXXXXX).c"
    echo "== [asan_focused] AOT-compiling (emit C only) bili fixture: ${BILI_MAIN}"
    ( cd "${BILI_PROJECT}" && "${XRAY_BIN}" build "${BILI_MAIN}" --native --c-only -o "${OUT_C}" )
    echo "== [asan_focused] bili AOT emit OK: $(wc -c <"${OUT_C}") bytes -> ${OUT_C}"
    rm -f "${OUT_C}"
else
    echo "!! [asan_focused] required bili fixture missing at ${BILI_MAIN}" >&2
    exit 1
fi

echo "== [asan_focused] PASS"
