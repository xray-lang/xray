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
# The lane configures XR_ASAN_BUILD_DIR itself rather than going through the
# asan-jit-debug preset, whose binaryDir is fixed and cannot take the caller's
# directory. An already-configured directory is reused as it stands, so the
# generator it was created with never has to match anything.
#
# Environment overrides:
#   XR_ASAN_JOBS          parallel build/test jobs (default: 8)
#   XR_ASAN_BUILD_DIR     ASan build directory (default: build-asan)
#   XR_ASAN_CTEST_REGEX   unit test name regex (default: ^test_)
#   XR_ASAN_CTEST_SERIAL_REGEX subprocess tests kept out of the saturated parallel lane
#   XR_ASAN_DIFF_REGEX    backend-diff subset regex (default: task190_.*_backend_diff)
#   XR_ASAN_XXHASH_MAIN   path to the xxhash port entry (default: repo-relative sibling)
#   XR_ASAN_BILI_MAIN     path to the committed bili-analysis-server fixture
#   XR_ASAN_SKIP_BUILD    if set to 1, reuse an existing ASan build

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${ROOT}"

# Default to all cores. This lane is registered RUN_SERIAL, so it owns the whole
# machine while it runs — there is nothing to over-subscribe against, and the
# ASan Debug rebuild is the single biggest cost in the lane. (Was hard-coded to
# 8, leaving most cores idle on an 18-core host.)
if command -v nproc >/dev/null 2>&1; then
    DEFAULT_JOBS="$(nproc)"
elif command -v sysctl >/dev/null 2>&1; then
    DEFAULT_JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 8)"
else
    DEFAULT_JOBS=8
fi
JOBS="${XR_ASAN_JOBS:-${DEFAULT_JOBS}}"
ASAN_BUILD="${XR_ASAN_BUILD_DIR:-build-asan}"
CTEST_REGEX="${XR_ASAN_CTEST_REGEX:-^test_}"
# Exclude native-toolchain / subprocess integration tests from the ASan
# *memory-safety* surface: they drive a full native AOT compile+link (assuming
# the default `build/` cache layout) or run the compiler as a subprocess under
# tight hardcoded timeouts, so they fail under ASan for environmental reasons
# (slowdown, cache mismatch) rather than any memory bug. The toolchain unit test
# also starts short-lived provider processes with the public 5s version-probe
# bound; run it serially below so an 8-way ASan lane cannot consume that budget
# through scheduler starvation. The ordinary ctest run still covers VM/native
# ABI parity. The pure C unit tests remain in scope.
CTEST_EXCLUDE="${XR_ASAN_CTEST_EXCLUDE:-native_error_abi|param_mode_diagnostics|param_contract|test_cli_toolchain}"
CTEST_SERIAL_REGEX="${XR_ASAN_CTEST_SERIAL_REGEX:-^test_cli_toolchain$}"
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
ASAN_CACHE="${ROOT}/${ASAN_BUILD}/CMakeCache.txt"

if [[ "${XR_ASAN_SKIP_BUILD:-0}" != "1" ]]; then
    # An already-configured directory is used as it stands, whatever generator
    # it was created with. Re-imposing one is what broke this lane: the Ninja
    # path configured the preset's hardcoded build-asan while every later step
    # used ${ASAN_BUILD}, so the knob configured one directory and built
    # another, and a directory created with a different generator failed
    # outright with no way to recover from the environment.
    if [[ -f "${ASAN_CACHE}" ]]; then
        echo "== [asan_focused] reusing existing configuration in ${ASAN_BUILD}"
    else
        # Ninja is the project's one build generator (see AGENTS.md). No Makefiles
        # fallback — fail with a clear install hint rather than silently building
        # something the rest of the toolchain does not expect.
        if ! command -v ninja >/dev/null 2>&1; then
            echo "!! [asan_focused] ninja not found — install it (brew install ninja / apt-get install ninja-build)" >&2
            exit 1
        fi
        echo "== [asan_focused] configuring ${ASAN_BUILD} (Ninja)"
        # Same cache variables as the asan-jit-debug preset in CMakePresets.json,
        # which stays the hand-run entry point. Keep the two in step; a preset
        # cannot take a caller-supplied binaryDir, so this lane cannot use it.
        cmake -S "${ROOT}" -B "${ROOT}/${ASAN_BUILD}" -G "Ninja" \
            -DCMAKE_BUILD_TYPE=Debug \
            -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
            -DENABLE_ASAN=ON -DENABLE_UBSAN=ON \
            -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    fi
    echo "== [asan_focused] building compiler + tests (ASan/UBSan)"
    cmake --build "${ROOT}/${ASAN_BUILD}" -j"${JOBS}"
fi

XRAY_BIN="${ROOT}/${ASAN_BUILD}/xray"
if [[ ! -x "${XRAY_BIN}" ]]; then
    echo "!! [asan_focused] ASan xray binary not found at ${XRAY_BIN}" >&2
    if [[ "${XR_ASAN_SKIP_BUILD:-0}" == "1" ]]; then
        echo "!! XR_ASAN_SKIP_BUILD=1 was set but no binary exists — build it first:" >&2
        echo "!!   cmake --build ${ASAN_BUILD} -j${JOBS}" >&2
    fi
    exit 1
fi

# When the build was skipped, the binary must not be older than the sources it
# is supposed to represent. A stale binary is worse than no binary: the lane
# would report a clean ASan result the current tree never earned. Fail loudly
# with the exact command to refresh it, rather than silently certifying old code.
if [[ "${XR_ASAN_SKIP_BUILD:-0}" == "1" ]]; then
    STALE_SRC="$(find src include stdlib CMakeLists.txt -type f -newer "${XRAY_BIN}" 2>/dev/null | head -1 || true)"
    if [[ -n "${STALE_SRC}" ]]; then
        echo "!! [asan_focused] the ASan binary is stale: ${STALE_SRC} is newer than ${XRAY_BIN}" >&2
        echo "!! rebuild it (cmake --build ${ASAN_BUILD} -j${JOBS}) or unset XR_ASAN_SKIP_BUILD" >&2
        exit 1
    fi
    echo "== [asan_focused] XR_ASAN_SKIP_BUILD=1: reusing up-to-date ASan binary"
fi

# Checked even when the build is skipped: the caller points XR_ASAN_BUILD_DIR at
# a directory it configured itself, and a lane that silently runs a non-ASan
# binary reports a clean result it never earned.
if [[ -f "${ASAN_CACHE}" ]] && ! grep -q '^ENABLE_ASAN:BOOL=ON' "${ASAN_CACHE}"; then
    echo "!! [asan_focused] ${ASAN_BUILD} is configured without ENABLE_ASAN=ON" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# 2. Unit tests under ASan/UBSan.
# ---------------------------------------------------------------------------
echo "== [asan_focused] running unit tests (regex: ${CTEST_REGEX}, exclude: ${CTEST_EXCLUDE})"
ctest --test-dir "${ASAN_BUILD}" --output-on-failure -j"${JOBS}" \
    -R "${CTEST_REGEX}" -E "${CTEST_EXCLUDE}" --timeout 300

echo "== [asan_focused] running subprocess-sensitive unit tests serially (regex: ${CTEST_SERIAL_REGEX})"
if ctest --test-dir "${ASAN_BUILD}" -N -R "${CTEST_SERIAL_REGEX}" 2>/dev/null | grep -qE "Test +#"; then
    ctest --test-dir "${ASAN_BUILD}" --output-on-failure -j1 \
        -R "${CTEST_SERIAL_REGEX}" --timeout 300
else
    echo "== [asan_focused] no serial unit tests matched ${CTEST_SERIAL_REGEX}; skipping"
fi

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
