#!/usr/bin/env bash
# Run Xray AOT benchmarks against C references and report progress toward the
# AOT C90 target.  Default mode is a baseline report; --gate turns ratio and
# code-shape expectations into failures.
#
# With --rust, benchmarks that ship a .rs reference are also compared against
# safe std-only Rust built at the same optimization tier.  Manifests gate the
# Rust column via min_rust_ratio; without it the Rust column is report-only.

set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BENCH_ROOT="$REPO_ROOT/tests/benchmarks/aot_c90"
XRAY_BIN="${XRAY_BIN:-$REPO_ROOT/build/xray}"
CC_BIN="${CC:-cc}"
RUSTC_BIN="${RUSTC:-rustc}"
OPT_LEVEL="3"
CPU=""
SAMPLES=31
FILTER=""
JSON_OUTPUT=""
GATE=false
KEEP=false
RUST=false

usage() {
    cat <<EOF
Usage: $0 [options]

Options:
  --xray-bin PATH    Xray executable (default: build/xray)
  --cc CC            C compiler (default: cc)
  --rust             Also benchmark Rust references (*.rs) when rustc exists
  --rustc PATH       Rust compiler (default: rustc)
  --opt LEVEL        xray build optimization level (default: 3)
  --cpu CPU          Tune host builds (Xray --cpu, C -march/-mcpu, Rust target-cpu)
  --samples N        Samples per runtime (default: 31)
  --quick            One sample, useful for smoke tests
  --bench LIST       Comma-separated benchmark basenames or relative names
  --json FILE        Write machine-readable JSON
  --gate             Fail when ratio/audit expectations are not met
  --keep             Keep temporary build directory
  -h, --help         Show this help

Default mode reports current distance from the target without failing on
performance or generated-code shape.  With --gate, ratio and generated-code
shape expectations fail immediately.  Build, run, and output mismatches still
fail.
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --xray-bin)
            XRAY_BIN="$2"
            shift 2
            ;;
        --cc)
            CC_BIN="$2"
            shift 2
            ;;
        --rust)
            RUST=true
            shift
            ;;
        --rustc)
            RUSTC_BIN="$2"
            shift 2
            ;;
        --opt)
            OPT_LEVEL="$2"
            shift 2
            ;;
        --cpu)
            CPU="$2"
            shift 2
            ;;
        --samples)
            SAMPLES="$2"
            shift 2
            ;;
        --quick)
            SAMPLES=1
            shift
            ;;
        --bench)
            FILTER="$2"
            shift 2
            ;;
        --json)
            JSON_OUTPUT="$2"
            shift 2
            ;;
        --gate)
            GATE=true
            shift
            ;;
        --keep)
            KEEP=true
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [ ! -x "$XRAY_BIN" ]; then
    echo "Missing executable xray binary: $XRAY_BIN" >&2
    exit 2
fi

if ! command -v "$CC_BIN" >/dev/null 2>&1; then
    echo "Missing C compiler: $CC_BIN" >&2
    exit 2
fi

if $RUST && ! command -v "$RUSTC_BIN" >/dev/null 2>&1; then
    echo "rustc not found ($RUSTC_BIN); Rust reference column disabled" >&2
    RUST=false
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 is required for timing and JSON output" >&2
    exit 2
fi

if [ ! -d "$BENCH_ROOT" ]; then
    echo "Missing benchmark directory: $BENCH_ROOT" >&2
    exit 2
fi

CC_CPU_ARG=""
if [ -n "$CPU" ]; then
    case "$(uname -m 2>/dev/null || echo unknown)" in
        arm64|aarch64|arm*)
            CC_CPU_ARG="-mcpu=$CPU"
            ;;
        *)
            CC_CPU_ARG="-march=$CPU"
            ;;
    esac
fi

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/xray_aot_c90.XXXXXX")"
RESULTS_TSV="$WORK_DIR/results.tsv"
DELIM=$'\034'
: > "$RESULTS_TSV"

if ! $KEEP; then
    trap 'rm -rf "$WORK_DIR"' EXIT
else
    echo "Work dir: $WORK_DIR"
fi

json_string() {
    local s=${1//\\/\\\\}
    s=${s//\"/\\\"}
    s=${s//$'\n'/\\n}
    s=${s//$'\r'/\\r}
    s=${s//$'\t'/\\t}
    printf '"%s"' "$s"
}

json_number_or_null() {
    if [[ ${1:-} =~ ^-?[0-9]+([.][0-9]+)?$ ]]; then
        printf '%s' "$1"
    else
        printf 'null'
    fi
}

median_of() {
    python3 - "$@" <<'PY'
import statistics
import sys
vals = [float(x) for x in sys.argv[1:] if x != ""]
if not vals:
    print("nan")
else:
    print(f"{statistics.median(vals):.6f}")
PY
}

ratio_of() {
    python3 - "$1" "$2" <<'PY'
import sys
c = float(sys.argv[1])
a = float(sys.argv[2])
print("nan" if a <= 0 else f"{c / a:.6f}")
PY
}

measure_ms() {
    local out_file=$1
    shift
    python3 - "$out_file" "$@" <<'PY'
import subprocess
import sys
import time

out_file = sys.argv[1]
cmd = sys.argv[2:]
start = time.perf_counter_ns()
with open(out_file, "wb") as out:
    rc = subprocess.run(cmd, stdout=out).returncode
end = time.perf_counter_ns()
print(f"{(end - start) / 1_000_000:.6f}")
sys.exit(rc)
PY
}

manifest_value() {
    local file=$1
    local key=$2
    [ -f "$file" ] || return 0
    awk -F= -v key="$key" '
        /^[[:space:]]*#/ { next }
        NF >= 2 {
            k=$1
            gsub(/[[:space:]]/, "", k)
            if (k == key) {
                v=$0
                sub(/^[^=]*=/, "", v)
                sub(/[[:space:]]*#.*/, "", v)
                gsub(/^[[:space:]]+|[[:space:]]+$/, "", v)
                print v
                exit
            }
        }
    ' "$file"
}

matches_filter() {
    local rel=$1
    local base=$2
    [ -z "$FILTER" ] && return 0
    IFS=',' read -r -a parts <<< "$FILTER"
    for item in "${parts[@]}"; do
        if [ "$item" = "$rel" ] || [ "$item" = "$base" ]; then
            return 0
        fi
    done
    return 1
}

metric_from_audit() {
    local file=$1
    local key=$2
    awk -F= -v key="$key" '$1 == key { print $2; exit }' "$file"
}

printf '=== Xray AOT C90 Benchmarks ===\n'
printf 'xray=%s\ncc=%s\nsamples=%s\ngate=%s\n' "$XRAY_BIN" "$CC_BIN" "$SAMPLES" "$GATE"
if [ -n "$CPU" ]; then
    printf 'cpu=%s\n' "$CPU"
fi
if $RUST; then
    printf 'rustc=%s (%s)\n' "$RUSTC_BIN" "$("$RUSTC_BIN" --version 2>/dev/null || echo unknown)"
fi
printf '\n'

PASS=0
FAIL=0
REPORT_ONLY_FAIL=0
FOUND=0

while IFS= read -r xr_file; do
    rel=${xr_file#"$BENCH_ROOT"/}
    rel_no_ext=${rel%.xr}
    base=$(basename "$xr_file" .xr)
    category=$(dirname "$rel")
    c_file="$BENCH_ROOT/$rel_no_ext.c"
    rs_file="$BENCH_ROOT/$rel_no_ext.rs"
    expect_file="$BENCH_ROOT/manifests/$base.expect"

    if ! matches_filter "$rel_no_ext" "$base"; then
        continue
    fi

    FOUND=$((FOUND + 1))
    bench_work="$WORK_DIR/${rel_no_ext//\//_}"
    mkdir -p "$bench_work"
    gen_c="$bench_work/$base.generated.c"
    aot_bin="$bench_work/$base.aot"
    c_bin="$bench_work/$base.c"
    aot_build_log="$bench_work/aot_build.log"
    c_build_log="$bench_work/c_build.log"
    audit_out="$bench_work/audit.txt"

    printf -- '--- %s ---\n' "$rel_no_ext"

    if [ ! -f "$c_file" ]; then
        echo "FAIL: missing C reference: $c_file"
        FAIL=$((FAIL + 1))
        continue
    fi

    xray_gen_args=(build --native -c -O "$OPT_LEVEL" -C "$CC_BIN" -o "$gen_c")
    if [ -n "$CPU" ]; then
        xray_gen_args+=(--cpu "$CPU")
    fi
    xray_gen_args+=("$xr_file")
    if ! "$XRAY_BIN" "${xray_gen_args[@]}" > "$aot_build_log" 2>&1; then
        echo "FAIL: AOT C generation failed"
        sed -n '1,20p' "$aot_build_log" | sed 's/^/  /'
        FAIL=$((FAIL + 1))
        continue
    fi

    xray_build_args=(build --native -O "$OPT_LEVEL" -C "$CC_BIN" -o "$aot_bin")
    if [ -n "$CPU" ]; then
        xray_build_args+=(--cpu "$CPU")
    fi
    xray_build_args+=("$xr_file")
    if ! "$XRAY_BIN" "${xray_build_args[@]}" >> "$aot_build_log" 2>&1; then
        echo "FAIL: AOT binary build failed"
        sed -n '1,20p' "$aot_build_log" | sed 's/^/  /'
        FAIL=$((FAIL + 1))
        continue
    fi

    c_build_args=(-O"$OPT_LEVEL")
    if [ -n "$CC_CPU_ARG" ]; then
        c_build_args+=("$CC_CPU_ARG")
    fi
    c_build_args+=("$c_file" -o "$c_bin" -lm)
    if ! "$CC_BIN" "${c_build_args[@]}" > "$c_build_log" 2>&1; then
        echo "FAIL: C reference build failed"
        sed -n '1,20p' "$c_build_log" | sed 's/^/  /'
        FAIL=$((FAIL + 1))
        continue
    fi

    rust_bin=""
    if $RUST && [ -f "$rs_file" ]; then
        rust_bin="$bench_work/$base.rust"
        rust_build_log="$bench_work/rust_build.log"
        rust_build_args=(-C "opt-level=$OPT_LEVEL" -C lto=fat -C panic=abort --edition 2021)
        if [ -n "$CPU" ]; then
            rust_build_args+=(-C "target-cpu=$CPU")
        fi
        rust_build_args+=("$rs_file" -o "$rust_bin")
        if ! "$RUSTC_BIN" "${rust_build_args[@]}" > "$rust_build_log" 2>&1; then
            echo "FAIL: Rust reference build failed"
            sed -n '1,20p' "$rust_build_log" | sed 's/^/  /'
            FAIL=$((FAIL + 1))
            continue
        fi
    fi

    audit_args=()
    if $GATE; then
        audit_args+=(--strict)
    fi
    if [ -f "$expect_file" ]; then
        audit_args+=(--expect "$expect_file")
    fi
    "$REPO_ROOT/scripts/check_aot_codegen_invariants.sh" "${audit_args[@]}" "$gen_c" > "$audit_out"
    audit_rc=$?
    if [ "$audit_rc" -ne 0 ]; then
        echo "FAIL: AOT generated-code audit failed"
        grep '^expectation_failure=' "$audit_out" | sed 's/^/  /' || true
        FAIL=$((FAIL + 1))
        continue
    fi

    c_times=()
    aot_times=()
    rust_times=()
    output_ok=1
    c_out_ref="$bench_work/c.ref.out"
    aot_out_ref="$bench_work/aot.ref.out"

    for ((i = 1; i <= SAMPLES; i++)); do
        c_out="$bench_work/c.$i.out"
        aot_out="$bench_work/aot.$i.out"
        c_ms=$(measure_ms "$c_out" "$c_bin") || {
            echo "FAIL: C reference run failed"
            output_ok=0
            break
        }
        aot_ms=$(measure_ms "$aot_out" "$aot_bin") || {
            echo "FAIL: AOT run failed"
            output_ok=0
            break
        }
        c_times+=("$c_ms")
        aot_times+=("$aot_ms")
        if [ -n "$rust_bin" ]; then
            rust_out="$bench_work/rust.$i.out"
            rust_ms=$(measure_ms "$rust_out" "$rust_bin") || {
                echo "FAIL: Rust reference run failed"
                output_ok=0
                break
            }
            rust_times+=("$rust_ms")
            if ! diff -u "$c_out" "$rust_out" > "$bench_work/rust_output.diff"; then
                echo "FAIL: C/Rust output mismatch"
                sed -n '1,20p' "$bench_work/rust_output.diff" | sed 's/^/  /'
                output_ok=0
                break
            fi
        fi
        if [ "$i" -eq 1 ]; then
            cp "$c_out" "$c_out_ref"
            cp "$aot_out" "$aot_out_ref"
        fi
        if ! diff -u "$c_out" "$aot_out" > "$bench_work/output.diff"; then
            echo "FAIL: C/AOT output mismatch"
            sed -n '1,20p' "$bench_work/output.diff" | sed 's/^/  /'
            output_ok=0
            break
        fi
    done

    if [ "$output_ok" -ne 1 ]; then
        FAIL=$((FAIL + 1))
        continue
    fi

    c_median=$(median_of "${c_times[@]}")
    aot_median=$(median_of "${aot_times[@]}")
    ratio=$(ratio_of "$c_median" "$aot_median")
    min_ratio=$(manifest_value "$expect_file" min_ratio)
    [ -z "$min_ratio" ] && min_ratio="0.90"
    ratio_pass=$(awk -v r="$ratio" -v m="$min_ratio" 'BEGIN { print (r >= m) ? 1 : 0 }')
    rust_median=""
    rust_ratio=""
    rust_ratio_pass=1
    min_rust_ratio=$(manifest_value "$expect_file" min_rust_ratio)
    if [ -n "$rust_bin" ] && [ "${#rust_times[@]}" -gt 0 ]; then
        rust_median=$(median_of "${rust_times[@]}")
        rust_ratio=$(ratio_of "$rust_median" "$aot_median")
        if [ -n "$min_rust_ratio" ]; then
            rust_ratio_pass=$(awk -v r="$rust_ratio" -v m="$min_rust_ratio" \
                'BEGIN { print (r >= m) ? 1 : 0 }')
        fi
    fi
    audit_pass=$(metric_from_audit "$audit_out" audit_pass)
    [ -z "$audit_pass" ] && audit_pass=1
    aot_size=$(wc -c < "$aot_bin" | tr -d ' ')
    c_size=$(wc -c < "$c_bin" | tr -d ' ')

    printf '  C median:   %s ms\n' "$c_median"
    printf '  AOT median: %s ms\n' "$aot_median"
    printf '  ratio:      %s (target >= %s)\n' "$ratio" "$min_ratio"
    if [ -n "$rust_median" ]; then
        printf '  Rust median: %s ms\n' "$rust_median"
        if [ -n "$min_rust_ratio" ]; then
            printf '  rust ratio: %s (target >= %s)\n' "$rust_ratio" "$min_rust_ratio"
        else
            printf '  rust ratio: %s (report only)\n' "$rust_ratio"
        fi
    fi
    printf '  audit:      %s\n' "$([ "$audit_pass" -eq 1 ] && echo pass || echo fail)"
    printf '  size:       C=%s AOT=%s bytes\n' "$c_size" "$aot_size"

    if [ "$audit_pass" -ne 1 ]; then
        grep '^expectation_failure=' "$audit_out" | sed 's/^/  /' || true
    fi

    status="pass"
    if [ "$ratio_pass" -ne 1 ] || [ "$audit_pass" -ne 1 ] || [ "$rust_ratio_pass" -ne 1 ]; then
        status="report_only_fail"
        REPORT_ONLY_FAIL=$((REPORT_ONLY_FAIL + 1))
    fi
    if $GATE && [ "$status" != "pass" ]; then
        FAIL=$((FAIL + 1))
    else
        PASS=$((PASS + 1))
    fi

    checksum=$(tr '\n' '|' < "$c_out_ref")
    printf '%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s\n' \
        "$rel_no_ext" "$DELIM" "$category" "$DELIM" "$status" "$DELIM" \
        "$c_median" "$DELIM" "$aot_median" "$DELIM" "$ratio" "$DELIM" \
        "$min_ratio" "$DELIM" "$audit_pass" "$DELIM" "$c_size" "$DELIM" \
        "$aot_size" "$DELIM" "$rust_median" "$DELIM" "$rust_ratio" "$DELIM" \
        "$checksum" >> "$RESULTS_TSV"
    echo
done < <(find "$BENCH_ROOT" -mindepth 2 -name '*.xr' | sort)

if [ "$FOUND" -eq 0 ]; then
    echo "No benchmarks matched."
    exit 2
fi

if [ -n "$JSON_OUTPUT" ]; then
    {
        printf '{\n'
        printf '  "xray": '; json_string "$XRAY_BIN"; printf ',\n'
        printf '  "cc": '; json_string "$CC_BIN"; printf ',\n'
        printf '  "cpu": '
        if [ -n "$CPU" ]; then
            json_string "$CPU"
        else
            printf 'null'
        fi
        printf ',\n'
        if $RUST; then
            printf '  "rustc": '; json_string "$("$RUSTC_BIN" --version 2>/dev/null || echo unknown)"; printf ',\n'
        fi
        printf '  "samples": %s,\n' "$SAMPLES"
        printf '  "gate": %s,\n' "$GATE"
        printf '  "benchmarks": [\n'
        first=true
        while IFS="$DELIM" read -r name category status c_ms aot_ms ratio min_ratio audit_pass c_size aot_size rust_ms rust_ratio checksum; do
            if ! $first; then
                printf ',\n'
            fi
            first=false
            printf '    {\n'
            printf '      "name": '; json_string "$name"; printf ',\n'
            printf '      "category": '; json_string "$category"; printf ',\n'
            printf '      "status": '; json_string "$status"; printf ',\n'
            printf '      "c_median_ms": '; json_number_or_null "$c_ms"; printf ',\n'
            printf '      "aot_median_ms": '; json_number_or_null "$aot_ms"; printf ',\n'
            printf '      "ratio": '; json_number_or_null "$ratio"; printf ',\n'
            printf '      "min_ratio": '; json_number_or_null "$min_ratio"; printf ',\n'
            printf '      "rust_median_ms": '; json_number_or_null "$rust_ms"; printf ',\n'
            printf '      "rust_ratio": '; json_number_or_null "$rust_ratio"; printf ',\n'
            printf '      "audit_pass": %s,\n' "$([ "$audit_pass" = "1" ] && echo true || echo false)"
            printf '      "c_size_bytes": '; json_number_or_null "$c_size"; printf ',\n'
            printf '      "aot_size_bytes": '; json_number_or_null "$aot_size"; printf ',\n'
            printf '      "checksum": '; json_string "$checksum"; printf '\n'
            printf '    }'
        done < "$RESULTS_TSV"
        printf '\n  ]\n'
        printf '}\n'
    } > "$JSON_OUTPUT"
    echo "JSON: $JSON_OUTPUT"
fi

echo "=== Results: $PASS completed, $FAIL failed, $REPORT_ONLY_FAIL below target/audit ==="

[ "$FAIL" -eq 0 ] && exit 0 || exit 1
