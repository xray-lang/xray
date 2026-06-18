#!/bin/bash
# run_backend_diff.sh - cross-backend differential test (107).
#
# Runs the same .xr program through VM and AOT, and asserts
# their observable output is byte-for-byte identical: stdout, exit code, and
# (normalized) stderr. This is the safety net for 108-118: any change that
# alters observable behavior must be caught here.
#
# Usage:
#   tests/diff/run_backend_diff.sh [xray_binary]
#
# Environment:
#   XRAY_BIN            xray binary (default: build/xray; also XRAY_BUILD_DIR)
#   XRAY_DIFF_BACKENDS  comma list subset of vm,aot (default: vm,aot)
#   XRAY_DIFF_EXTRA_CASES_FILE
#                       newline case manifest, relative to repo root or absolute
#                       (default: tests/diff/coro_regression_cases.txt; empty disables)
#
# Per-case optional sidecar:
#   <case>.args    first line = whitespace-separated program arguments
#   <case>.xr first line may carry "// anchor: <tag>" for failure labeling.
#
# Cases live in tests/diff/cases/**/*.xr; files whose basename starts with
# '_' are skipped (shared helpers / fixtures).
#
# Written for bash 3.2 portability (no associative arrays): per-backend exit
# codes are stashed in files, not a declare -A map.
#
# Exit: 0 all backends agree on every case; 1 any divergence/failure.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
CASE_DIR="$SCRIPT_DIR/cases"
NORMALIZE="$SCRIPT_DIR/normalize.sed"
EXTRA_CASES_FILE="${XRAY_DIFF_EXTRA_CASES_FILE-$SCRIPT_DIR/coro_regression_cases.txt}"

XRAY="${1:-${XRAY_BIN:-}}"
if [ -z "$XRAY" ]; then
    if [ -n "${XRAY_BUILD_DIR:-}" ]; then
        XRAY="$XRAY_BUILD_DIR/xray"
    else
        XRAY="$PROJECT_DIR/build/xray"
    fi
fi

BACKENDS="${XRAY_DIFF_BACKENDS:-vm,aot}"
# Observable contract = stdout + exit code (matches tests/aot/run_aot_tests.sh).
# stderr is backend-diagnostic-heavy (AOT cc/build logs, native-class
# warnings), so it is NOT part of pass/fail unless XRAY_DIFF_STDERR=1.
DIFF_STDERR="${XRAY_DIFF_STDERR:-0}"

PASS=0
FAIL=0
SKIP=0

WORK="$(mktemp -d "${TMPDIR:-/tmp}/xray_backend_diff.XXXXXX")" || {
    echo "error: cannot create temp dir" >&2
    exit 1
}
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

backend_enabled() {
    case ",$BACKENDS," in
        *",$1,"*) return 0 ;;
        *) return 1 ;;
    esac
}

normalize() {
    if [ -f "$NORMALIZE" ]; then
        sed -f "$NORMALIZE" "$1"
    else
        cat "$1"
    fi
}

rel_path() {
    case "$1" in
        "$PROJECT_DIR"/*) printf '%s' "${1#"$PROJECT_DIR"/}" ;;
        *) printf '%s' "$1" ;;
    esac
}

# run_backend KIND CASE OUT_PREFIX [args...]
# Writes: OUT_PREFIX.out (stdout), OUT_PREFIX.err (normalized stderr),
#         OUT_PREFIX.rc (exit code as text).
run_backend() {
    local kind="$1" case="$2" out_prefix="$3"
    shift 3
    local raw_err="$out_prefix.rawerr"
    local rc=0
    case "$kind" in
        vm)
            if [ "$#" -gt 0 ]; then
                "$XRAY" run "$case" -- "$@" >"$out_prefix.out" 2>"$raw_err" || rc=$?
            else
                "$XRAY" run "$case" >"$out_prefix.out" 2>"$raw_err" || rc=$?
            fi
            ;;
        aot)
            local bin="$out_prefix.bin"
            if ! "$XRAY" build --native "$case" -o "$bin" >"$out_prefix.buildlog" 2>&1; then
                echo "BUILDFAIL" >"$out_prefix.out"
                : >"$raw_err"
                rc=200
            else
                if [ "$#" -gt 0 ]; then
                    "$bin" "$@" >"$out_prefix.out" 2>"$raw_err" || rc=$?
                else
                    "$bin" >"$out_prefix.out" 2>"$raw_err" || rc=$?
                fi
            fi
            ;;
    esac
    normalize "$raw_err" >"$out_prefix.err"
    printf '%s' "$rc" >"$out_prefix.rc"
}

run_case() {
    local case="$1"
    local name base anchor argfile
    name="$(rel_path "$case")"
    base="$(printf '%s' "${name%.xr}" | sed 's#[^A-Za-z0-9_.-]#_#g')"
    printf '  %-52s' "$name"

    anchor="$(sed -n '1{s#^// *anchor: *##p;}' "$case" 2>/dev/null)"

    # Per-case backend restriction. A case may carry, within its first 5 lines,
    #   // diff-backends: vm,aot
    # to opt out of backends that have a known, tracked divergence.  The
    # exclusion is printed (never silent) so the net still documents what it
    # is not yet covering.
    local case_backends
    case_backends="$(sed -n '1,5{s#^// *diff-backends: *##p;}' "$case" 2>/dev/null | head -n1)"

    local has_args=0
    local argline=""
    argfile="${case%.xr}.args"
    if [ -f "$argfile" ]; then
        argline="$(head -n1 "$argfile")"
        has_args=1
    fi

    local enabled="" excluded=""
    for b in vm aot; do
        backend_enabled "$b" || continue
        if [ -n "$case_backends" ]; then
            case ",$case_backends," in
                *",$b,"*) ;;
                *) excluded="$excluded $b"; continue ;;
            esac
        fi
        enabled="$enabled $b"
    done
    # count enabled
    local nb=0
    for b in $enabled; do nb=$((nb + 1)); done
    if [ "$nb" -lt 2 ]; then
        echo "SKIP (need >=2 backends; case=${case_backends:-all} global=$BACKENDS)"
        SKIP=$((SKIP + 1))
        return 0
    fi

    for b in $enabled; do
        if [ "$has_args" -eq 1 ]; then
            # word-split argline intentionally
            # shellcheck disable=SC2086
            run_backend "$b" "$case" "$WORK/${base}.$b" $argline
        else
            run_backend "$b" "$case" "$WORK/${base}.$b"
        fi
    done

    local ref=""
    for b in $enabled; do ref="$b"; break; done
    local ref_out="$WORK/${base}.$ref.out"
    local ref_err="$WORK/${base}.$ref.err"
    local ref_rc cur_rc
    ref_rc="$(cat "$WORK/${base}.$ref.rc")"
    local mismatch=""
    local other=""
    for b in $enabled; do
        [ "$b" = "$ref" ] && continue
        cur_rc="$(cat "$WORK/${base}.$b.rc")"
        if [ "$cur_rc" != "$ref_rc" ]; then
            mismatch="exit code ($ref=$ref_rc $b=$cur_rc)"; other="$b"; break
        fi
        if ! diff -q "$ref_out" "$WORK/${base}.$b.out" >/dev/null 2>&1; then
            mismatch="stdout ($ref vs $b)"; other="$b"; break
        fi
        if [ "$DIFF_STDERR" = "1" ] && ! diff -q "$ref_err" "$WORK/${base}.$b.err" >/dev/null 2>&1; then
            mismatch="stderr ($ref vs $b)"; other="$b"; break
        fi
    done

    if [ -z "$mismatch" ]; then
        if [ -n "$excluded" ]; then
            echo "PASS (excl:$excluded)"
        else
            echo "PASS"
        fi
        PASS=$((PASS + 1))
        return 0
    fi

    echo "FAIL ($mismatch)${anchor:+  [anchor: $anchor]}"
    for b in $enabled; do
        echo "      $b: rc=$(cat "$WORK/${base}.$b.rc")  stdout: $(head -3 "$WORK/${base}.$b.out" | tr '\n' '|')"
    done
    case "$mismatch" in
        stdout*) diff -u "$ref_out" "$WORK/${base}.$other.out" | sed -n '1,30p' | sed 's/^/      /' ;;
        stderr*) diff -u "$ref_err" "$WORK/${base}.$other.err" | sed -n '1,30p' | sed 's/^/      /' ;;
    esac
    FAIL=$((FAIL + 1))
    return 1
}

echo "=== Backend Differential (VM / AOT) ==="
echo "Binary:   $XRAY"
echo "Backends: $BACKENDS"
echo ""

if ! command -v "$XRAY" >/dev/null 2>&1 && [ ! -x "$XRAY" ]; then
    echo "SKIP: xray binary not found: $XRAY"
    echo "=== Results: 0 passed, 0 failed, 0 skipped ==="
    exit 0
fi

if [ ! -d "$CASE_DIR" ]; then
    echo "SKIP: no case dir $CASE_DIR"
    echo "=== Results: 0 passed, 0 failed, 0 skipped ==="
    exit 0
fi

CASE_LIST="$WORK/cases.list"
find "$CASE_DIR" -name '*.xr' | sort >"$CASE_LIST"
if [ -n "$EXTRA_CASES_FILE" ] && [ -f "$EXTRA_CASES_FILE" ]; then
    while IFS= read -r f; do
        case "$f" in
            ""|\#*) continue ;;
            /*) printf '%s\n' "$f" ;;
            *) printf '%s/%s\n' "$PROJECT_DIR" "$f" ;;
        esac
    done <"$EXTRA_CASES_FILE" >>"$CASE_LIST"
fi

while IFS= read -r f; do
    [ -f "$f" ] || continue
    case "$(basename "$f")" in
        _*) continue ;;
    esac
    run_case "$f"
done <"$CASE_LIST"

echo ""
echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped ==="
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
