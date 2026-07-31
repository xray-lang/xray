#!/bin/bash
# run_panic_report_tests.sh
#
# Uncaught panics must read the same on the VM and AOT backends: one line
#
#     [Uncaught Panic] E<code>: <message>
#
# built from the fault's error code and message (shared/xr_panic_report.h).
#
# This is a content gate, not a parity gate. The VM/AOT differential net runs
# with stderr comparison off by default (XRAY_DIFF_STDERR=0), so a regression
# that changed the wording on BOTH backends would still pass it. Only an
# assertion on the actual text keeps the report honest — the same reason the
# value-return channel has run_uncaught_error_tests.sh.
#
# It also pins the two policy decisions that make cross-backend parity possible:
#   - the stack trace is opt-in (XRAY_BACKTRACE), absent by default, so the
#     default report matches a backend that carries no unwind state;
#   - colour is TTY-gated, so piped output is plain on both backends.
#
# Usage: tests/cli/run_panic_report_tests.sh [xray_binary]

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
XRAY="${1:-${XRAY_BIN:-$PROJECT_DIR/build/xray}}"

if [ ! -x "$XRAY" ]; then
    echo "FAIL: xray not found at $XRAY" >&2
    exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/xray_panic_report.XXXXXX")" || {
    echo "FAIL: cannot create temporary directory" >&2
    exit 1
}
trap 'rm -rf "$WORK"' EXIT

PASS=0
FAIL=0

# strip ANSI so the assertions are colour-independent.
strip_ansi() { sed -E $'s/\x1b\\[[0-9;]*m//g'; }

# check_run <label> <xray-args...> -- <expected-exit> <expected-stdout>
#           <expected-stderr-substring> <forbidden-stderr-substring|"">
# Runs "$XRAY <args>" piped (non-TTY) and asserts on the result.
check_run() {
    local label="$1"; shift
    local -a run_args=()
    while [ "$1" != "--" ]; do run_args+=("$1"); shift; done
    shift
    local want_rc="$1" want_out="$2" want_err="$3" forbid_err="$4"

    "$XRAY" "${run_args[@]}" > "$WORK/out" 2> "$WORK/err"
    local got_rc=$?
    local got_out got_err
    got_out="$(cat "$WORK/out")"
    got_err="$(strip_ansi < "$WORK/err")"

    if [ "$got_rc" != "$want_rc" ]; then
        echo "FAIL: $label — exit $got_rc, want $want_rc" >&2
        FAIL=$((FAIL + 1)); return
    fi
    if [ "$got_out" != "$want_out" ]; then
        echo "FAIL: $label — stdout '$got_out', want '$want_out'" >&2
        FAIL=$((FAIL + 1)); return
    fi
    case "$got_err" in
        *"$want_err"*) ;;
        *) echo "FAIL: $label — stderr lacks '$want_err'; got '$got_err'" >&2
           FAIL=$((FAIL + 1)); return ;;
    esac
    if [ -n "$forbid_err" ]; then
        case "$got_err" in
            *"$forbid_err"*)
                echo "FAIL: $label — stderr must not contain '$forbid_err'; got '$got_err'" >&2
                FAIL=$((FAIL + 1)); return ;;
        esac
    fi
    # Piped output must never carry raw ANSI escapes.
    if grep -q $'\x1b' "$WORK/err"; then
        echo "FAIL: $label — piped stderr contains ANSI escapes" >&2
        FAIL=$((FAIL + 1)); return
    fi
    echo "PASS: $label"
    PASS=$((PASS + 1))
}

cat > "$WORK/div.xr" <<'EOF'
var z = 0
print("before")
print(1 / z)
EOF

cat > "$WORK/oob.xr" <<'EOF'
var xs = [1, 2, 3]
print("before")
xs[9] = 0
EOF

# ---- VM: canonical panic report, no trace by default ----------------------
check_run "vm div-by-zero panic report" run "$WORK/div.xr" -- \
    1 "before" "[Uncaught Panic] E0420: division by zero" "Stack trace:"

check_run "vm array-oob panic report" run "$WORK/oob.xr" -- \
    1 "before" "[Uncaught Panic] E0430: array index out of range: 9 (length 3)" "Stack trace:"

# ---- VM: XRAY_BACKTRACE adds the opt-in trace -----------------------------
XRAY_BACKTRACE=1 "$XRAY" run "$WORK/div.xr" > "$WORK/out" 2> "$WORK/err"
bt_rc=$?
bt_err="$(strip_ansi < "$WORK/err")"
if [ "$bt_rc" = 1 ] &&
   printf '%s' "$bt_err" | grep -q '\[Uncaught Panic\] E0420: division by zero' &&
   printf '%s' "$bt_err" | grep -q 'Stack trace:'; then
    echo "PASS: vm XRAY_BACKTRACE adds stack trace"
    PASS=$((PASS + 1))
else
    echo "FAIL: vm XRAY_BACKTRACE — want report + 'Stack trace:'; got '$bt_err'" >&2
    FAIL=$((FAIL + 1))
fi

# ---- AOT parity (skips when no native toolchain provider is READY) ---------
if "$XRAY" build --native "$WORK/div.xr" -o "$WORK/div_native" > "$WORK/build.log" 2>&1 &&
        [ -x "$WORK/div_native" ]; then
    "$WORK/div_native" > "$WORK/out" 2> "$WORK/err"
    a_rc=$?
    a_out="$(cat "$WORK/out")"
    a_err="$(strip_ansi < "$WORK/err")"
    if [ "$a_rc" = 1 ] && [ "$a_out" = "before" ] &&
       printf '%s' "$a_err" | grep -q '\[Uncaught Panic\] E0420: division by zero' &&
       ! printf '%s' "$a_err" | grep -q 'Stack trace:'; then
        echo "PASS: aot div-by-zero matches the VM panic report"
        PASS=$((PASS + 1))
    else
        echo "FAIL: aot div-by-zero — rc=$a_rc out='$a_out' err='$a_err'" >&2
        FAIL=$((FAIL + 1))
    fi
else
    echo "SKIP: aot div-by-zero — no native toolchain provider available"
fi

echo "----------------------------------------"
echo "Panic report: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
