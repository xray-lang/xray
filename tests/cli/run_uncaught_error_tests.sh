#!/bin/bash
# run_uncaught_error_tests.sh
#
# Spec §8.1.1: an uncaught value-return error prints a diagnostic to stderr.
# Two wordings, and they must stay byte-identical across the VM and AOT:
#
#   [Uncaught Error] <value>                  — the top-level program
#   [Uncaught Error in go coroutine] <value>  — a dropped fire-and-forget `go`
#
# This is a content gate, not a parity gate. The VM/AOT differential net in
# tests/diff cannot catch a regression here. The top-level shape regressed once
# already — an elided root (a program that spawns nothing) runs on the native
# stack with no main coroutine and never reached run_finalize() where the report
# lives. The `go` shape exits 0 on BOTH backends — the diagnostic is the only
# observable difference, and stderr comparison is off by default
# (XRAY_DIFF_STDERR=0), so a backend that prints nothing still "agrees". Only an
# assertion on the actual text keeps either failure from going silent.
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

# ===== Top-level (elided / scheduler) value-return error, exits 1 ==========
# An elided root spawns nothing, so it runs on the native stack with no main
# coroutine — the shape that regressed. The scheduled variant (a `go` forces a
# main coroutine) takes the other finalization path, so both stay covered.
TOP_ERR='[Uncaught Error] TopErr.Failed("top-level")'

cat > "$WORK/elided.xr" <<'EOF'
enum TopErr { Failed(reason: string) }

fn run() {
    throw TopErr.Failed("top-level")
}

print("before")
run()
EOF

cat > "$WORK/scheduled.xr" <<'EOF'
enum TopErr { Failed(reason: string) }

fn run() {
    throw TopErr.Failed("top-level")
}

go { }
print("before")
run()
EOF

cat > "$WORK/caught.xr" <<'EOF'
enum TopErr { Failed(reason: string) }

fn run() {
    throw TopErr.Failed("top-level")
}

try { run() } catch (e) { print("caught") }
EOF

# ===== Dropped fire-and-forget `go`, exits 0 ===============================
# No Task handle and no enclosing scope, so nothing is left to observe the
# error. Both backends must report it, and both exit 0 — the spawning program
# itself completed normally.
cat > "$WORK/in_go.xr" <<'EOF'
enum GoErr { Failed(reason: string) }

print("before")
go { throw GoErr.Failed("in-go") }
EOF
GO_ERR='[Uncaught Error in go coroutine] GoErr.Failed("in-go")'

# The two shapes that must stay SILENT. Both reach the same finalization path
# with the same error, and only the observer differs — they are what keeps the
# report from being written as an unconditional print.
#
#   (a) a Task handle: the error is delivered to whoever awaits it.
cat > "$WORK/observed.xr" <<'EOF'
enum GoErr { Failed(reason: string) }

fn fail() -> int {
    throw GoErr.Failed("observed")
    return 0
}

var task = go fail()
match (task.awaitResult()) {
    TaskResult.Failed(err) -> print("caught")
    _ -> print("unexpected")
}
EOF

#   (b) a parent scope: the scope collects the terminal state at scope exit.
cat > "$WORK/scoped.xr" <<'EOF'
enum GoErr { Failed(reason: string) }

fn fail() {
    throw GoErr.Failed("scoped")
}

scope {
    go fail()
}
print("after")
EOF

# ---- VM ------------------------------------------------------------------
"$XRAY" run "$WORK/elided.xr" > "$WORK/vm_elided.out" 2> "$WORK/vm_elided.err"
check "vm elided root reports uncaught error" 1 "before" "$TOP_ERR" \
    "$?" "$WORK/vm_elided.out" "$WORK/vm_elided.err"

"$XRAY" run "$WORK/scheduled.xr" > "$WORK/vm_sched.out" 2> "$WORK/vm_sched.err"
check "vm scheduled root reports uncaught error" 1 "before" "$TOP_ERR" \
    "$?" "$WORK/vm_sched.out" "$WORK/vm_sched.err"

"$XRAY" run "$WORK/caught.xr" > "$WORK/vm_caught.out" 2> "$WORK/vm_caught.err"
check "vm caught error prints nothing" 0 "caught" "" \
    "$?" "$WORK/vm_caught.out" "$WORK/vm_caught.err"

"$XRAY" run "$WORK/in_go.xr" > "$WORK/vm_go.out" 2> "$WORK/vm_go.err"
check "vm dropped go coroutine reports uncaught error" 0 "before" "$GO_ERR" \
    "$?" "$WORK/vm_go.out" "$WORK/vm_go.err"

"$XRAY" run "$WORK/observed.xr" > "$WORK/vm_obs.out" 2> "$WORK/vm_obs.err"
check "vm awaited go coroutine stays silent" 0 "caught" "" \
    "$?" "$WORK/vm_obs.out" "$WORK/vm_obs.err"

"$XRAY" run "$WORK/scoped.xr" > "$WORK/vm_scoped.out" 2> "$WORK/vm_scoped.err"
check "vm scoped go coroutine stays silent" 0 "after" "" \
    "$?" "$WORK/vm_scoped.out" "$WORK/vm_scoped.err"

# ---- AOT -----------------------------------------------------------------
# The native toolchain is optional in this gate: when no provider is READY the
# AOT leg is skipped rather than failed, matching the other AOT-optional tests.
if "$XRAY" build --native "$WORK/elided.xr" -o "$WORK/elided_native" \
        > "$WORK/build_elided.log" 2>&1 && [ -x "$WORK/elided_native" ]; then
    "$WORK/elided_native" > "$WORK/aot_elided.out" 2> "$WORK/aot_elided.err"
    check "aot elided root reports uncaught error" 1 "before" "$TOP_ERR" \
        "$?" "$WORK/aot_elided.out" "$WORK/aot_elided.err"

    if "$XRAY" build --native "$WORK/in_go.xr" -o "$WORK/in_go_native" \
            > "$WORK/build.log" 2>&1 && [ -x "$WORK/in_go_native" ]; then
        "$WORK/in_go_native" > "$WORK/aot_go.out" 2> "$WORK/aot_go.err"
        check "aot dropped go coroutine reports uncaught error" 0 "before" "$GO_ERR" \
            "$?" "$WORK/aot_go.out" "$WORK/aot_go.err"

        # Byte-for-byte, not just "both contain the message": the whole point of
        # routing AOT through its own printer is that the wording cannot drift.
        if diff "$WORK/vm_go.err" "$WORK/aot_go.err" > /dev/null 2>&1; then
            echo "PASS: vm and aot stderr are byte-identical"
            PASS=$((PASS + 1))
        else
            echo "FAIL: vm and aot stderr differ" >&2
            diff "$WORK/vm_go.err" "$WORK/aot_go.err" >&2
            FAIL=$((FAIL + 1))
        fi
    else
        echo "FAIL: aot dropped go coroutine — native build failed" >&2
        FAIL=$((FAIL + 1))
    fi

    if "$XRAY" build --native "$WORK/observed.xr" -o "$WORK/observed_native" \
            > "$WORK/build_observed.log" 2>&1 && [ -x "$WORK/observed_native" ]; then
        "$WORK/observed_native" > "$WORK/aot_obs.out" 2> "$WORK/aot_obs.err"
        check "aot awaited go coroutine stays silent" 0 "caught" "" \
            "$?" "$WORK/aot_obs.out" "$WORK/aot_obs.err"
    else
        echo "FAIL: aot awaited go coroutine — native build failed" >&2
        FAIL=$((FAIL + 1))
    fi

    if "$XRAY" build --native "$WORK/scoped.xr" -o "$WORK/scoped_native" \
            > "$WORK/build_scoped.log" 2>&1 && [ -x "$WORK/scoped_native" ]; then
        "$WORK/scoped_native" > "$WORK/aot_scoped.out" 2> "$WORK/aot_scoped.err"
        check "aot scoped go coroutine stays silent" 0 "after" "" \
            "$?" "$WORK/aot_scoped.out" "$WORK/aot_scoped.err"
    else
        echo "FAIL: aot scoped go coroutine — native build failed" >&2
        FAIL=$((FAIL + 1))
    fi
else
    echo "SKIP: aot legs — no native toolchain provider available"
fi

echo "----------------------------------------"
echo "Uncaught error diagnostics: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
