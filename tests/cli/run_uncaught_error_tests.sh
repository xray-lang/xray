#!/bin/bash
# run_uncaught_error_tests.sh
#
# Spec §8.1.1: an uncaught top-level value-return error prints
# "[Uncaught Error] <enum value>" to stderr and exits 1.
#
# This is a content gate, not a parity gate. The VM/AOT differential net in
# tests/diff cannot catch a regression here: when the diagnostic went missing
# it went missing on BOTH backends, so they still agreed byte-for-byte. Only an
# assertion on the actual text keeps the failure from going silent again.
#
# The elided-root shape is the one that regressed. A program that spawns
# nothing gets root_representation = XR_ROOT_ELIDED, so the VM runs it on the
# native stack with no main coroutine and never reaches the coroutine backend's
# run_finalize() — where the diagnostic lives. The `go`-carrying variant takes
# the scheduler path instead, and is covered here so the two stay in step.
#
# Usage: tests/cli/run_uncaught_error_tests.sh [xray_binary]

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
XRAY="${1:-${XRAY_BIN:-$PROJECT_DIR/build/xray}}"

if [ ! -x "$XRAY" ]; then
    echo "FAIL: xray not found at $XRAY" >&2
    exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/xray_uncaught_error.XXXXXX")" || {
    echo "FAIL: cannot create temporary directory" >&2
    exit 1
}
trap 'rm -rf "$WORK"' EXIT

PASS=0
FAIL=0

# check <label> <expected-exit> <expected-stdout> <expected-stderr-substring>
#       <actual-exit> <stdout-file> <stderr-file>
check() {
    local label="$1" want_rc="$2" want_out="$3" want_err="$4"
    local got_rc="$5" out_file="$6" err_file="$7"
    local got_out got_err
    got_out="$(cat "$out_file")"
    got_err="$(cat "$err_file")"

    if [ "$got_rc" != "$want_rc" ]; then
        echo "FAIL: $label — exit code $got_rc, want $want_rc" >&2
        FAIL=$((FAIL + 1))
        return
    fi
    if [ "$got_out" != "$want_out" ]; then
        echo "FAIL: $label — stdout '$got_out', want '$want_out'" >&2
        FAIL=$((FAIL + 1))
        return
    fi
    case "$got_err" in
        *"$want_err"*) ;;
        *)
            echo "FAIL: $label — stderr does not contain '$want_err'" >&2
            echo "  actual stderr: '$got_err'" >&2
            FAIL=$((FAIL + 1))
            return
            ;;
    esac
    echo "PASS: $label"
    PASS=$((PASS + 1))
}

# ---- Case 1: elided root (no spawn) — the shape that regressed -------------
cat > "$WORK/elided.xr" <<'EOF'
enum TopErr { Failed(reason: string) }

fn run() {
    throw TopErr.Failed("top-level")
}

print("before")
run()
EOF

"$XRAY" run "$WORK/elided.xr" > "$WORK/elided.out" 2> "$WORK/elided.err"
check "vm elided root reports uncaught error" 1 "before" \
    '[Uncaught Error] TopErr.Failed("top-level")' \
    "$?" "$WORK/elided.out" "$WORK/elided.err"

# ---- Case 2: scheduler-backed root (a `go` forces a main coroutine) --------
cat > "$WORK/scheduled.xr" <<'EOF'
enum TopErr { Failed(reason: string) }

fn run() {
    throw TopErr.Failed("top-level")
}

go { }
print("before")
run()
EOF

"$XRAY" run "$WORK/scheduled.xr" > "$WORK/scheduled.out" 2> "$WORK/scheduled.err"
check "vm scheduled root reports uncaught error" 1 "before" \
    '[Uncaught Error] TopErr.Failed("top-level")' \
    "$?" "$WORK/scheduled.out" "$WORK/scheduled.err"

# ---- Case 3: dropped fire-and-forget `go` keeps its own wording ------------
cat > "$WORK/in_go.xr" <<'EOF'
enum GoErr { Failed(reason: string) }

print("before")
go { throw GoErr.Failed("in-go") }
EOF

"$XRAY" run "$WORK/in_go.xr" > "$WORK/in_go.out" 2> "$WORK/in_go.err"
check "vm dropped go coroutine reports uncaught error" 0 "before" \
    '[Uncaught Error in go coroutine] GoErr.Failed("in-go")' \
    "$?" "$WORK/in_go.out" "$WORK/in_go.err"

# ---- Case 4: a caught error must stay silent ------------------------------
cat > "$WORK/caught.xr" <<'EOF'
enum TopErr { Failed(reason: string) }

fn run() {
    throw TopErr.Failed("top-level")
}

try { run() } catch (e) { print("caught") }
EOF

"$XRAY" run "$WORK/caught.xr" > "$WORK/caught.out" 2> "$WORK/caught.err"
check "vm caught error prints nothing" 0 "caught" "" \
    "$?" "$WORK/caught.out" "$WORK/caught.err"

# ---- Case 5: AOT parity ---------------------------------------------------
# The native toolchain is optional in this gate: when no provider is READY the
# AOT leg is skipped rather than failed, matching the other AOT-optional tests.
if "$XRAY" build --native "$WORK/elided.xr" -o "$WORK/elided_native" \
        > "$WORK/build.log" 2>&1 && [ -x "$WORK/elided_native" ]; then
    "$WORK/elided_native" > "$WORK/native.out" 2> "$WORK/native.err"
    check "aot elided root reports uncaught error" 1 "before" \
        '[Uncaught Error] TopErr.Failed("top-level")' \
        "$?" "$WORK/native.out" "$WORK/native.err"
else
    echo "SKIP: aot elided root — no native toolchain provider available"
fi

echo "----------------------------------------"
echo "Uncaught error diagnostics: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
