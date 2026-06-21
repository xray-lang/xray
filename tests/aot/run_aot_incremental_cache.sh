#!/bin/bash
# AOT per-module incremental object-cache tests (114 separate compilation).
#
# Usage:
#   ./tests/aot/run_aot_incremental_cache.sh [xray_binary]
#
# Verifies that a multi-module native build only recompiles the modules whose
# generated C actually changed, reusing cached objects for the rest:
#   - changing a dependency's function body recompiles the dependency but the
#     dependent hits the cache and is relinked against the new object (no stale
#     result);
#   - adding an unrelated export to a dependency leaves dependents cache-valid
#     (stable cross-module names + per-import forward declarations);
#   - editing only the entry leaves the dependency cache-valid.
#
# Environment:
#   XRAY_AOT_TEST_OPT   native C compiler optimization level for this
#                       correctness/cache gate (default: 0)

set -u

XRAY="${1:-./build/xray}"
WORK="${TMPDIR:-/tmp}/xray_aot_incr_cache_$$"
CACHE="$WORK/.cache"
LIB="$WORK/mathlib.xr"
APP="$WORK/app.xr"
AOT_OPT_LEVEL="${XRAY_AOT_TEST_OPT:-0}"

PASS=0
FAIL=0

mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

echo "=== AOT Incremental Cache Tests ==="
echo "Binary: $XRAY"
echo "Work:   $WORK"
echo "AOT opt: -O$AOT_OPT_LEVEL"
echo ""

if [ ! -x "$XRAY" ]; then
    echo "FAIL: xray binary not executable: $XRAY"
    exit 1
fi

record_pass() {
    echo "  PASS: $1"
    PASS=$((PASS + 1))
}

record_fail() {
    echo "  FAIL: $1"
    FAIL=$((FAIL + 1))
}

# build_log <out_binary> <log_file> [extra args...]
# Exercises the explicit --cache-dir flag (precedence over $XRAY_CACHE_DIR).
build_log() {
    local out="$1" log="$2"
    shift 2
    "$XRAY" build --native -O "$AOT_OPT_LEVEL" --verbose --cache-dir "$CACHE" "$@" -o "$out" \
        "$APP" >"$log" 2>&1
}

# expect_state <log> <module> <compiling|hit> <test-name>
expect_state() {
    local log="$1" mod="$2" want="$3" name="$4"
    if [ "$want" = "compiling" ]; then
        if grep -q "compiling: $mod " "$log"; then
            record_pass "$name: $mod recompiled"
        else
            record_fail "$name: expected $mod to recompile"
            sed 's/^/      /' "$log" | grep -E 'compiling|cache hit' || true
        fi
    else
        if grep -q "cache hit: $mod " "$log"; then
            record_pass "$name: $mod cache hit"
        else
            record_fail "$name: expected $mod cache hit"
            sed 's/^/      /' "$log" | grep -E 'compiling|cache hit' || true
        fi
    fi
}

# expect_output <binary> <expected> <test-name>
expect_output() {
    local bin="$1" want="$2" name="$3" got
    got="$("$bin" 2>/dev/null)"
    if [ "$got" = "$want" ]; then
        record_pass "$name: output '$want'"
    else
        record_fail "$name: output '$got' != '$want'"
    fi
}

cat > "$APP" <<'XR_EOF'
import { triple } from "./mathlib"

print(triple(14))
XR_EOF

# --- 1. Cold build: every module compiles. ------------------------------------
cat > "$LIB" <<'XR_EOF'
export fn triple(x: int) -> int {
    return x * 3
}
XR_EOF

build_log "$WORK/app1" "$WORK/log1" || { echo "FAIL: cold build failed"; sed 's/^/  /' "$WORK/log1"; exit 1; }
expect_state "$WORK/log1" mathlib compiling "cold"
expect_state "$WORK/log1" app compiling "cold"
expect_output "$WORK/app1" "42" "cold"

# --- 2. Change a used dependency's body: dep recompiles, dependent hits. -------
cat > "$LIB" <<'XR_EOF'
export fn triple(x: int) -> int {
    return x * 5
}
XR_EOF

build_log "$WORK/app2" "$WORK/log2" || { echo "FAIL: build 2 failed"; sed 's/^/  /' "$WORK/log2"; exit 1; }
expect_state "$WORK/log2" mathlib compiling "body-change"
expect_state "$WORK/log2" app hit "body-change"
# Relinked against the new object: the dependent reflects the new behavior even
# though it was not recompiled (cache hit must not be stale).
expect_output "$WORK/app2" "70" "body-change"

# --- 3. Add an unrelated export: dependent stays cache-valid. ------------------
cat > "$LIB" <<'XR_EOF'
export fn triple(x: int) -> int {
    return x * 5
}

export fn quad(x: int) -> int {
    return x * 4
}
XR_EOF

build_log "$WORK/app3" "$WORK/log3" || { echo "FAIL: build 3 failed"; sed 's/^/  /' "$WORK/log3"; exit 1; }
expect_state "$WORK/log3" mathlib compiling "add-export"
expect_state "$WORK/log3" app hit "add-export"
expect_output "$WORK/app3" "70" "add-export"

# --- 4. Edit only the entry: the untouched dependency stays cache-valid. -------
cat > "$APP" <<'XR_EOF'
import { triple } from "./mathlib"

print(triple(14))
print(triple(2))
XR_EOF

build_log "$WORK/app4" "$WORK/log4" || { echo "FAIL: build 4 failed"; sed 's/^/  /' "$WORK/log4"; exit 1; }
expect_state "$WORK/log4" mathlib hit "entry-only"
expect_state "$WORK/log4" app compiling "entry-only"
expect_output "$WORK/app4" "$(printf '70\n10')" "entry-only"

# --- 5. --rebuild forces every module to recompile, ignoring the warm cache. --
build_log "$WORK/app5" "$WORK/log5" --rebuild || { echo "FAIL: build 5 failed"; sed 's/^/  /' "$WORK/log5"; exit 1; }
expect_state "$WORK/log5" mathlib compiling "rebuild"
expect_state "$WORK/log5" app compiling "rebuild"
expect_output "$WORK/app5" "$(printf '70\n10')" "rebuild"

# --- 6. Cross-module class: constructor/method symbols are order-independent, so
#        adding an unrelated member to the class module leaves the importer
#        cache-valid (it is relinked against the new object). ------------------
CLIB="$WORK/shape.xr"
CAPP="$WORK/capp.xr"
CCACHE="$WORK/.ccache"
cat > "$CAPP" <<'XR_EOF'
import { Box } from "./shape"

let b = new Box(4)
print(b.area())
XR_EOF
cat > "$CLIB" <<'XR_EOF'
export class Box {
    side: int
    constructor(s: int) {
        this.side = s
    }
    area() -> int {
        return this.side * this.side
    }
}
XR_EOF

"$XRAY" build --native -O "$AOT_OPT_LEVEL" --verbose --cache-dir "$CCACHE" -o "$WORK/capp1" \
    "$CAPP" >"$WORK/clog1" 2>&1 \
    || { echo "FAIL: class build 1 failed"; sed 's/^/  /' "$WORK/clog1"; exit 1; }
if grep -q "compiling: capp " "$WORK/clog1" && grep -q "compiling: shape " "$WORK/clog1"; then
    record_pass "class-cold: both modules compiled"
else
    record_fail "class-cold: expected both modules to compile"
fi
expect_output "$WORK/capp1" "16" "class-cold"

cat > "$CLIB" <<'XR_EOF'
export class Box {
    side: int
    constructor(s: int) {
        this.side = s
    }
    area() -> int {
        return this.side * this.side
    }
    perimeter() -> int {
        return this.side * 4
    }
}
XR_EOF

"$XRAY" build --native -O "$AOT_OPT_LEVEL" --verbose --cache-dir "$CCACHE" -o "$WORK/capp2" \
    "$CAPP" >"$WORK/clog2" 2>&1 \
    || { echo "FAIL: class build 2 failed"; sed 's/^/  /' "$WORK/clog2"; exit 1; }
expect_state "$WORK/clog2" shape compiling "class-add-method"
expect_state "$WORK/clog2" capp hit "class-add-method"
expect_output "$WORK/capp2" "16" "class-add-method"

# --- 7. Whole-program LTO mode: separate cache namespace, still incremental,
#        and a correct cross-module binary. -----------------------------------
LCACHE="$WORK/.ltocache"
"$XRAY" build --native -O "$AOT_OPT_LEVEL" --lto --verbose --cache-dir "$LCACHE" \
    -o "$WORK/lapp1" "$APP" >"$WORK/llog1" 2>&1 \
    || { echo "FAIL: lto build 1 failed"; sed 's/^/  /' "$WORK/llog1"; exit 1; }
expect_state "$WORK/llog1" mathlib compiling "lto-cold"
expect_output "$WORK/lapp1" "$(printf '70\n10')" "lto-cold"
"$XRAY" build --native -O "$AOT_OPT_LEVEL" --lto --verbose --cache-dir "$LCACHE" \
    -o "$WORK/lapp2" "$APP" >"$WORK/llog2" 2>&1 \
    || { echo "FAIL: lto build 2 failed"; sed 's/^/  /' "$WORK/llog2"; exit 1; }
expect_state "$WORK/llog2" mathlib hit "lto-warm"
expect_state "$WORK/llog2" app hit "lto-warm"
expect_output "$WORK/lapp2" "$(printf '70\n10')" "lto-warm"

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
