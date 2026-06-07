#!/usr/bin/env bash
# AOT generated-C invariant scanner.
#
# The scanner reports code-shape metrics that matter for the C90 AOT plan:
# XrValue leakage, boxing/unboxing, tagged arithmetic helpers, typed-array
# runtime switches, and VM/JIT header dependencies.  With --expect it also
# compares metrics against an expectation file.  With --strict, expectation
# failures make the script exit non-zero.

set -u

usage() {
    cat <<EOF
Usage: $0 [--expect FILE] [--strict] <generated.c>...

Expectation file format:
  # comments are ignored
  hot_function=^module_run_[0-9]+$
  hot_region=typed_array_raw_access
  hot_region_start=^[[:space:]]*int64_t v[0-9]+ = \(int64_t\)_ad[0-9]+\[phi[0-9]+\];
  hot_region_end=^[[:space:]]*goto L[0-9]+;
  hot_xrvalue_local_count=0
  hot_region_xrvalue_local_count=0
  xrvalue_local_count=0
  box_count=0
  runtime_arith_calls=0
  runtime_map_calls=0
  runtime_set_calls=0
  runtime_property_calls=0
  typed_array_data_field_load_count=1
  typed_array_direct_data_index_count=0
  typed_array_per_iter_len_store_count=0

Each expectation means metric <= value. Unknown keys are ignored so the same
manifest can also carry benchmark fields such as min_ratio.
EOF
}

EXPECT_FILE=""
STRICT=false
FILES=()

while [ "$#" -gt 0 ]; do
    case "$1" in
        --expect)
            if [ "$#" -lt 2 ]; then
                echo "Missing value for --expect" >&2
                exit 2
            fi
            EXPECT_FILE="$2"
            shift 2
            ;;
        --strict)
            STRICT=true
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            break
            ;;
        -*)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
        *)
            FILES+=("$1")
            shift
            ;;
    esac
done

if [ "${#FILES[@]}" -eq 0 ]; then
    usage >&2
    exit 2
fi

for f in "${FILES[@]}"; do
    if [ ! -f "$f" ]; then
        echo "Missing generated C file: $f" >&2
        exit 2
    fi
done

manifest_value() {
    local key=$1
    [ -n "$EXPECT_FILE" ] || return 0
    [ -r "$EXPECT_FILE" ] || return 0
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
    ' "$EXPECT_FILE"
}

HOT_FUNCTION=$(manifest_value hot_function)
HOT_REGION=$(manifest_value hot_region)
HOT_REGION_START=$(manifest_value hot_region_start)
HOT_REGION_END=$(manifest_value hot_region_end)
HOT_WORK_DIR=""
HOT_FILES=()
HOT_FILE_COUNT=0
HOT_REGION_FILES=()
HOT_REGION_FILE_COUNT=0
HOT_FUNCTION_MISSING=0
HOT_REGION_MISSING=0
HOT_REGION_INCOMPLETE=0

if [ -n "$HOT_REGION" ]; then
    case "$HOT_REGION" in
        typed_array_raw_access)
            if [ -z "$HOT_REGION_START" ]; then
                HOT_REGION_START='XR_AOT_HOT_REGION_BEGIN[[:space:]]+typed_array_raw_access'
            fi
            if [ -z "$HOT_REGION_END" ]; then
                HOT_REGION_END='XR_AOT_HOT_REGION_END[[:space:]]+typed_array_raw_access'
            fi
            ;;
        *)
            HOT_REGION_INCOMPLETE=1
            ;;
    esac
fi

cleanup_hot_work_dir() {
    if [ -n "$HOT_WORK_DIR" ]; then
        rm -rf "$HOT_WORK_DIR"
    fi
}

extract_hot_functions() {
    local pattern=$1
    local input=$2
    local output=$3
    awk -v pat="$pattern" '
        function count_char(s, c,    i, n) {
            n = 0
            for (i = 1; i <= length(s); i++) {
                if (substr(s, i, 1) == c)
                    n++
            }
            return n
        }
        function function_name(line,    name) {
            name = line
            sub(/^static[[:space:]]+/, "", name)
            sub(/\(.*/, "", name)
            sub(/.*[[:space:]*]/, "", name)
            return name
        }
        {
            if (!in_func) {
                if ($0 ~ /^static[[:space:]].*\)[[:space:]]*\{/) {
                    name = function_name($0)
                    if (name ~ pat) {
                        in_func = 1
                        matched++
                        depth = count_char($0, "{") - count_char($0, "}")
                        print
                        if (depth <= 0)
                            in_func = 0
                    }
                }
            } else {
                print
                depth += count_char($0, "{") - count_char($0, "}")
                if (depth <= 0)
                    in_func = 0
            }
        }
        END { if (matched == 0) exit 3 }
    ' "$input" > "$output"
}

extract_hot_regions() {
    local start_pattern=$1
    local end_pattern=$2
    local input=$3
    local output=$4
    awk -v start_pat="$start_pattern" -v end_pat="$end_pattern" '
        {
            if (!in_region) {
                if ($0 ~ start_pat) {
                    in_region = 1
                    matched++
                    print
                    if ($0 ~ end_pat)
                        in_region = 0
                }
            } else {
                print
                if ($0 ~ end_pat)
                    in_region = 0
            }
        }
        END {
            if (in_region)
                exit 4
            if (matched == 0)
                exit 3
        }
    ' "$input" > "$output"
}

if [ -n "$HOT_FUNCTION" ] || [ -n "$HOT_REGION_START" ] || [ -n "$HOT_REGION_END" ]; then
    HOT_WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/xray_aot_hot.XXXXXX")"
    trap cleanup_hot_work_dir EXIT
fi

if [ -n "$HOT_FUNCTION" ]; then
    idx=0
    for f in "${FILES[@]}"; do
        out="$HOT_WORK_DIR/hot_$idx.c"
        if extract_hot_functions "$HOT_FUNCTION" "$f" "$out"; then
            HOT_FILES+=("$out")
            HOT_FILE_COUNT=$((HOT_FILE_COUNT + 1))
        fi
        idx=$((idx + 1))
    done
    if [ "$HOT_FILE_COUNT" -eq 0 ]; then
        HOT_FUNCTION_MISSING=1
    fi
fi

if [ -n "$HOT_REGION_START" ] || [ -n "$HOT_REGION_END" ]; then
    if [ -z "$HOT_REGION_START" ] || [ -z "$HOT_REGION_END" ]; then
        HOT_REGION_INCOMPLETE=1
    else
        idx=0
        if [ "$HOT_FILE_COUNT" -gt 0 ]; then
            REGION_INPUTS=("${HOT_FILES[@]}")
        else
            REGION_INPUTS=("${FILES[@]}")
        fi
        for f in "${REGION_INPUTS[@]}"; do
            out="$HOT_WORK_DIR/hot_region_$idx.c"
            if extract_hot_regions "$HOT_REGION_START" "$HOT_REGION_END" "$f" "$out"; then
                HOT_REGION_FILES+=("$out")
                HOT_REGION_FILE_COUNT=$((HOT_REGION_FILE_COUNT + 1))
            else
                if [ -s "$out" ]; then
                    HOT_REGION_INCOMPLETE=1
                    HOT_REGION_FILES+=("$out")
                    HOT_REGION_FILE_COUNT=$((HOT_REGION_FILE_COUNT + 1))
                fi
            fi
            idx=$((idx + 1))
        done
        if [ "$HOT_REGION_FILE_COUNT" -eq 0 ]; then
            HOT_REGION_MISSING=1
        fi
    fi
fi

count_pattern_in() {
    local pattern=$1
    shift
    if [ "$#" -eq 0 ]; then
        printf '0\n'
        return
    fi
    grep -E -- "$pattern" "$@" 2>/dev/null | wc -l | tr -d ' '
}

count_pattern() {
    local pattern=$1
    count_pattern_in "$pattern" "${FILES[@]}"
}

count_hot_pattern() {
    local pattern=$1
    if [ "$HOT_FILE_COUNT" -eq 0 ]; then
        printf '0\n'
        return
    fi
    count_pattern_in "$pattern" "${HOT_FILES[@]}"
}

count_hot_region_pattern() {
    local pattern=$1
    if [ "$HOT_REGION_FILE_COUNT" -eq 0 ]; then
        printf '0\n'
        return
    fi
    count_pattern_in "$pattern" "${HOT_REGION_FILES[@]}"
}

xrvalue_count=$(count_pattern '\bXrValue\b')
xrvalue_local_count=$(count_pattern '(^|[({;=,[:space:]])XrValue[[:space:]]+[*]?[A-Za-z_][A-Za-z0-9_]*')
box_count=$(count_pattern 'XR_FROM_[A-Z0-9_]*[[:space:]]*\(|xr_box_[A-Za-z0-9_]*[[:space:]]*\(|XI_BOX')
unbox_count=$(count_pattern 'XR_TO_[A-Z0-9_]*[[:space:]]*\(|xr_unbox_[A-Za-z0-9_]*[[:space:]]*\(|XI_UNBOX')
runtime_arith_calls=$(count_pattern 'xrt_(add|sub|mul|div|mod)[[:space:]]*\(')
runtime_int_checked_arith_calls=$(count_pattern 'xrt_int_(div|mod)[[:space:]]*\(')
typed_array_runtime_calls=$(count_pattern 'xr_typed_(get|set)[[:space:]]*\(')
typed_array_bounds_check_count=$(count_pattern 'if[[:space:]]*\(_idx[[:space:]]*<[[:space:]]*0\)|_idx[[:space:]]*>=[[:space:]]*0[[:space:]]*&&[[:space:]]*_idx[[:space:]]*<[[:space:]]*_a->len')
typed_array_capacity_check_count=$(count_pattern '_a->len[[:space:]]*>=[[:space:]]*_a->cap|XRT_REALLOC[[:space:]]*\(_a->data')
typed_array_data_field_load_count=$(count_pattern '->data')
typed_array_direct_data_index_count=$(count_pattern '->data\)\[')
typed_array_per_iter_len_store_count=$(count_pattern '_a->len[[:space:]]*=')
runtime_array_calls=$(count_pattern 'xrt_array_[A-Za-z0-9_]*[[:space:]]*\(')
runtime_map_calls=$(count_pattern 'xrt_map_[A-Za-z0-9_]*[[:space:]]*\(')
runtime_set_calls=$(count_pattern 'xrt_set_[A-Za-z0-9_]*[[:space:]]*\(')
runtime_property_calls=$(count_pattern 'xrt_(getprop|setprop)[[:space:]]*\(')
dynamic_dispatch_calls=$(count_pattern 'xrt_(call_method|method|vcall|invoke|dispatch)[A-Za-z0-9_]*[[:space:]]*\(')
pending_error_check_count=$(count_pattern 'xrt_has_pending_error[[:space:]]*\(')
vm_jit_include_count=$(count_pattern '^#include[[:space:]]+["<].*(src/)?(vm|jit|xvm|xm_)')

hot_function_count=0
if [ "$HOT_FILE_COUNT" -gt 0 ]; then
    hot_function_count=$(count_hot_pattern '^static[[:space:]].*\)[[:space:]]*\{')
fi
hot_xrvalue_count=$(count_hot_pattern '\bXrValue\b')
hot_xrvalue_local_count=$(count_hot_pattern '(^|[({;=,[:space:]])XrValue[[:space:]]+[*]?[A-Za-z_][A-Za-z0-9_]*')
hot_box_count=$(count_hot_pattern 'XR_FROM_[A-Z0-9_]*[[:space:]]*\(|xr_box_[A-Za-z0-9_]*[[:space:]]*\(|XI_BOX')
hot_unbox_count=$(count_hot_pattern 'XR_TO_[A-Z0-9_]*[[:space:]]*\(|xr_unbox_[A-Za-z0-9_]*[[:space:]]*\(|XI_UNBOX')
hot_runtime_arith_calls=$(count_hot_pattern 'xrt_(add|sub|mul|div|mod)[[:space:]]*\(')
hot_runtime_int_checked_arith_calls=$(count_hot_pattern 'xrt_int_(div|mod)[[:space:]]*\(')
hot_typed_array_runtime_calls=$(count_hot_pattern 'xr_typed_(get|set)[[:space:]]*\(')
hot_typed_array_bounds_check_count=$(count_hot_pattern 'if[[:space:]]*\(_idx[[:space:]]*<[[:space:]]*0\)|_idx[[:space:]]*>=[[:space:]]*0[[:space:]]*&&[[:space:]]*_idx[[:space:]]*<[[:space:]]*_a->len')
hot_typed_array_capacity_check_count=$(count_hot_pattern '_a->len[[:space:]]*>=[[:space:]]*_a->cap|XRT_REALLOC[[:space:]]*\(_a->data')
hot_typed_array_data_field_load_count=$(count_hot_pattern '->data')
hot_typed_array_direct_data_index_count=$(count_hot_pattern '->data\)\[')
hot_typed_array_per_iter_len_store_count=$(count_hot_pattern '_a->len[[:space:]]*=')
hot_int64_phi_count=$(count_hot_pattern '^[[:space:]]*int64_t[[:space:]]+phi[0-9]+[[:space:]]*=')
hot_if_count=$(count_hot_pattern '^[[:space:]]*if[[:space:]]*\(')
hot_while_count=$(count_hot_pattern '^[[:space:]]*while[[:space:]]*\(')
hot_runtime_array_calls=$(count_hot_pattern 'xrt_array_[A-Za-z0-9_]*[[:space:]]*\(')
hot_runtime_map_calls=$(count_hot_pattern 'xrt_map_[A-Za-z0-9_]*[[:space:]]*\(')
hot_runtime_set_calls=$(count_hot_pattern 'xrt_set_[A-Za-z0-9_]*[[:space:]]*\(')
hot_runtime_property_calls=$(count_hot_pattern 'xrt_(getprop|setprop)[[:space:]]*\(')
hot_dynamic_dispatch_calls=$(count_hot_pattern 'xrt_(call_method|method|vcall|invoke|dispatch)[A-Za-z0-9_]*[[:space:]]*\(')
hot_pending_error_check_count=$(count_hot_pattern 'xrt_has_pending_error[[:space:]]*\(')
hot_region_count=0
if [ "$HOT_REGION_FILE_COUNT" -gt 0 ]; then
    hot_region_count=$(count_hot_region_pattern "$HOT_REGION_START")
fi
hot_region_xrvalue_count=$(count_hot_region_pattern '\bXrValue\b')
hot_region_xrvalue_local_count=$(count_hot_region_pattern '(^|[({;=,[:space:]])XrValue[[:space:]]+[*]?[A-Za-z_][A-Za-z0-9_]*')
hot_region_box_count=$(count_hot_region_pattern 'XR_FROM_[A-Z0-9_]*[[:space:]]*\(|xr_box_[A-Za-z0-9_]*[[:space:]]*\(|XI_BOX')
hot_region_unbox_count=$(count_hot_region_pattern 'XR_TO_[A-Z0-9_]*[[:space:]]*\(|xr_unbox_[A-Za-z0-9_]*[[:space:]]*\(|XI_UNBOX')
hot_region_runtime_arith_calls=$(count_hot_region_pattern 'xrt_(add|sub|mul|div|mod)[[:space:]]*\(')
hot_region_runtime_int_checked_arith_calls=$(count_hot_region_pattern 'xrt_int_(div|mod)[[:space:]]*\(')
hot_region_typed_array_runtime_calls=$(count_hot_region_pattern 'xr_typed_(get|set)[[:space:]]*\(')
hot_region_typed_array_bounds_check_count=$(count_hot_region_pattern 'if[[:space:]]*\(_idx[[:space:]]*<[[:space:]]*0\)|_idx[[:space:]]*>=[[:space:]]*0[[:space:]]*&&[[:space:]]*_idx[[:space:]]*<[[:space:]]*_a->len')
hot_region_typed_array_capacity_check_count=$(count_hot_region_pattern '_a->len[[:space:]]*>=[[:space:]]*_a->cap|XRT_REALLOC[[:space:]]*\(_a->data')
hot_region_typed_array_data_field_load_count=$(count_hot_region_pattern '->data')
hot_region_typed_array_direct_data_index_count=$(count_hot_region_pattern '->data\)\[')
hot_region_typed_array_per_iter_len_store_count=$(count_hot_region_pattern '_a->len[[:space:]]*=')
hot_region_runtime_array_calls=$(count_hot_region_pattern 'xrt_array_[A-Za-z0-9_]*[[:space:]]*\(')
hot_region_runtime_map_calls=$(count_hot_region_pattern 'xrt_map_[A-Za-z0-9_]*[[:space:]]*\(')
hot_region_runtime_set_calls=$(count_hot_region_pattern 'xrt_set_[A-Za-z0-9_]*[[:space:]]*\(')
hot_region_runtime_property_calls=$(count_hot_region_pattern 'xrt_(getprop|setprop)[[:space:]]*\(')
hot_region_dynamic_dispatch_calls=$(count_hot_region_pattern 'xrt_(call_method|method|vcall|invoke|dispatch)[A-Za-z0-9_]*[[:space:]]*\(')
hot_region_pending_error_check_count=$(count_hot_region_pattern 'xrt_has_pending_error[[:space:]]*\(')

metric_value() {
    case "$1" in
        xrvalue_count) printf '%s\n' "$xrvalue_count" ;;
        xrvalue_local_count) printf '%s\n' "$xrvalue_local_count" ;;
        box_count) printf '%s\n' "$box_count" ;;
        unbox_count) printf '%s\n' "$unbox_count" ;;
        runtime_arith_calls) printf '%s\n' "$runtime_arith_calls" ;;
        runtime_int_checked_arith_calls) printf '%s\n' "$runtime_int_checked_arith_calls" ;;
        typed_array_runtime_calls) printf '%s\n' "$typed_array_runtime_calls" ;;
        typed_array_bounds_check_count) printf '%s\n' "$typed_array_bounds_check_count" ;;
        typed_array_capacity_check_count) printf '%s\n' "$typed_array_capacity_check_count" ;;
        typed_array_data_field_load_count) printf '%s\n' "$typed_array_data_field_load_count" ;;
        typed_array_direct_data_index_count) printf '%s\n' "$typed_array_direct_data_index_count" ;;
        typed_array_per_iter_len_store_count) printf '%s\n' "$typed_array_per_iter_len_store_count" ;;
        runtime_array_calls) printf '%s\n' "$runtime_array_calls" ;;
        runtime_map_calls) printf '%s\n' "$runtime_map_calls" ;;
        runtime_set_calls) printf '%s\n' "$runtime_set_calls" ;;
        runtime_property_calls) printf '%s\n' "$runtime_property_calls" ;;
        dynamic_dispatch_calls) printf '%s\n' "$dynamic_dispatch_calls" ;;
        pending_error_check_count) printf '%s\n' "$pending_error_check_count" ;;
        vm_jit_include_count) printf '%s\n' "$vm_jit_include_count" ;;
        hot_function_count) printf '%s\n' "$hot_function_count" ;;
        hot_xrvalue_count) printf '%s\n' "$hot_xrvalue_count" ;;
        hot_xrvalue_local_count) printf '%s\n' "$hot_xrvalue_local_count" ;;
        hot_box_count) printf '%s\n' "$hot_box_count" ;;
        hot_unbox_count) printf '%s\n' "$hot_unbox_count" ;;
        hot_runtime_arith_calls) printf '%s\n' "$hot_runtime_arith_calls" ;;
        hot_runtime_int_checked_arith_calls) printf '%s\n' "$hot_runtime_int_checked_arith_calls" ;;
        hot_typed_array_runtime_calls) printf '%s\n' "$hot_typed_array_runtime_calls" ;;
        hot_typed_array_bounds_check_count) printf '%s\n' "$hot_typed_array_bounds_check_count" ;;
        hot_typed_array_capacity_check_count) printf '%s\n' "$hot_typed_array_capacity_check_count" ;;
        hot_typed_array_data_field_load_count) printf '%s\n' "$hot_typed_array_data_field_load_count" ;;
        hot_typed_array_direct_data_index_count) printf '%s\n' "$hot_typed_array_direct_data_index_count" ;;
        hot_typed_array_per_iter_len_store_count) printf '%s\n' "$hot_typed_array_per_iter_len_store_count" ;;
        hot_int64_phi_count) printf '%s\n' "$hot_int64_phi_count" ;;
        hot_if_count) printf '%s\n' "$hot_if_count" ;;
        hot_while_count) printf '%s\n' "$hot_while_count" ;;
        hot_runtime_array_calls) printf '%s\n' "$hot_runtime_array_calls" ;;
        hot_runtime_map_calls) printf '%s\n' "$hot_runtime_map_calls" ;;
        hot_runtime_set_calls) printf '%s\n' "$hot_runtime_set_calls" ;;
        hot_runtime_property_calls) printf '%s\n' "$hot_runtime_property_calls" ;;
        hot_dynamic_dispatch_calls) printf '%s\n' "$hot_dynamic_dispatch_calls" ;;
        hot_pending_error_check_count) printf '%s\n' "$hot_pending_error_check_count" ;;
        hot_region_count) printf '%s\n' "$hot_region_count" ;;
        hot_region_xrvalue_count) printf '%s\n' "$hot_region_xrvalue_count" ;;
        hot_region_xrvalue_local_count) printf '%s\n' "$hot_region_xrvalue_local_count" ;;
        hot_region_box_count) printf '%s\n' "$hot_region_box_count" ;;
        hot_region_unbox_count) printf '%s\n' "$hot_region_unbox_count" ;;
        hot_region_runtime_arith_calls) printf '%s\n' "$hot_region_runtime_arith_calls" ;;
        hot_region_runtime_int_checked_arith_calls) printf '%s\n' "$hot_region_runtime_int_checked_arith_calls" ;;
        hot_region_typed_array_runtime_calls) printf '%s\n' "$hot_region_typed_array_runtime_calls" ;;
        hot_region_typed_array_bounds_check_count) printf '%s\n' "$hot_region_typed_array_bounds_check_count" ;;
        hot_region_typed_array_capacity_check_count) printf '%s\n' "$hot_region_typed_array_capacity_check_count" ;;
        hot_region_typed_array_data_field_load_count) printf '%s\n' "$hot_region_typed_array_data_field_load_count" ;;
        hot_region_typed_array_direct_data_index_count) printf '%s\n' "$hot_region_typed_array_direct_data_index_count" ;;
        hot_region_typed_array_per_iter_len_store_count) printf '%s\n' "$hot_region_typed_array_per_iter_len_store_count" ;;
        hot_region_runtime_array_calls) printf '%s\n' "$hot_region_runtime_array_calls" ;;
        hot_region_runtime_map_calls) printf '%s\n' "$hot_region_runtime_map_calls" ;;
        hot_region_runtime_set_calls) printf '%s\n' "$hot_region_runtime_set_calls" ;;
        hot_region_runtime_property_calls) printf '%s\n' "$hot_region_runtime_property_calls" ;;
        hot_region_dynamic_dispatch_calls) printf '%s\n' "$hot_region_dynamic_dispatch_calls" ;;
        hot_region_pending_error_check_count) printf '%s\n' "$hot_region_pending_error_check_count" ;;
        *) return 1 ;;
    esac
}

audit_pass=1
expect_checked=0

if [ "$HOT_FUNCTION_MISSING" -ne 0 ]; then
    audit_pass=0
    printf 'expectation_failure=hot_function actual=0 expected_match=%s\n' "$HOT_FUNCTION"
fi

if [ "$HOT_REGION_INCOMPLETE" -ne 0 ]; then
    audit_pass=0
    printf 'expectation_failure=hot_region actual=incomplete expected=start_and_end_match\n'
fi

if [ "$HOT_REGION_MISSING" -ne 0 ]; then
    audit_pass=0
    printf 'expectation_failure=hot_region actual=0 expected_match=%s\n' "$HOT_REGION_START"
fi

if [ -n "$EXPECT_FILE" ]; then
    if [ ! -r "$EXPECT_FILE" ]; then
        echo "Missing expectation file: $EXPECT_FILE" >&2
        exit 2
    fi

    while IFS= read -r raw_line || [ -n "$raw_line" ]; do
        line=${raw_line%%#*}
        line=$(printf '%s' "$line" | tr -d '\r' |
            sed 's/^[[:space:]]*//; s/[[:space:]]*$//')
        [ -z "$line" ] && continue
        key=${line%%=*}
        expected=${line#*=}
        key=${key//[[:space:]]/}
        expected=${expected//[[:space:]]/}

        actual=$(metric_value "$key" || true)
        [ -z "$actual" ] && continue
        expect_checked=$((expect_checked + 1))
        if ! awk -v a="$actual" -v e="$expected" 'BEGIN { exit !(a <= e) }'; then
            audit_pass=0
            printf 'expectation_failure=%s actual=%s expected_max=%s\n' "$key" "$actual" "$expected"
        fi
    done < "$EXPECT_FILE"
fi

cat <<EOF
xrvalue_count=$xrvalue_count
xrvalue_local_count=$xrvalue_local_count
box_count=$box_count
unbox_count=$unbox_count
runtime_arith_calls=$runtime_arith_calls
runtime_int_checked_arith_calls=$runtime_int_checked_arith_calls
typed_array_runtime_calls=$typed_array_runtime_calls
typed_array_bounds_check_count=$typed_array_bounds_check_count
typed_array_capacity_check_count=$typed_array_capacity_check_count
typed_array_data_field_load_count=$typed_array_data_field_load_count
typed_array_direct_data_index_count=$typed_array_direct_data_index_count
typed_array_per_iter_len_store_count=$typed_array_per_iter_len_store_count
runtime_array_calls=$runtime_array_calls
runtime_map_calls=$runtime_map_calls
runtime_set_calls=$runtime_set_calls
runtime_property_calls=$runtime_property_calls
dynamic_dispatch_calls=$dynamic_dispatch_calls
pending_error_check_count=$pending_error_check_count
vm_jit_include_count=$vm_jit_include_count
hot_function_count=$hot_function_count
hot_xrvalue_count=$hot_xrvalue_count
hot_xrvalue_local_count=$hot_xrvalue_local_count
hot_box_count=$hot_box_count
hot_unbox_count=$hot_unbox_count
hot_runtime_arith_calls=$hot_runtime_arith_calls
hot_runtime_int_checked_arith_calls=$hot_runtime_int_checked_arith_calls
hot_typed_array_runtime_calls=$hot_typed_array_runtime_calls
hot_typed_array_bounds_check_count=$hot_typed_array_bounds_check_count
hot_typed_array_capacity_check_count=$hot_typed_array_capacity_check_count
hot_typed_array_data_field_load_count=$hot_typed_array_data_field_load_count
hot_typed_array_direct_data_index_count=$hot_typed_array_direct_data_index_count
hot_typed_array_per_iter_len_store_count=$hot_typed_array_per_iter_len_store_count
hot_int64_phi_count=$hot_int64_phi_count
hot_if_count=$hot_if_count
hot_while_count=$hot_while_count
hot_runtime_array_calls=$hot_runtime_array_calls
hot_runtime_map_calls=$hot_runtime_map_calls
hot_runtime_set_calls=$hot_runtime_set_calls
hot_runtime_property_calls=$hot_runtime_property_calls
hot_dynamic_dispatch_calls=$hot_dynamic_dispatch_calls
hot_pending_error_check_count=$hot_pending_error_check_count
hot_region_count=$hot_region_count
hot_region_xrvalue_count=$hot_region_xrvalue_count
hot_region_xrvalue_local_count=$hot_region_xrvalue_local_count
hot_region_box_count=$hot_region_box_count
hot_region_unbox_count=$hot_region_unbox_count
hot_region_runtime_arith_calls=$hot_region_runtime_arith_calls
hot_region_runtime_int_checked_arith_calls=$hot_region_runtime_int_checked_arith_calls
hot_region_typed_array_runtime_calls=$hot_region_typed_array_runtime_calls
hot_region_typed_array_bounds_check_count=$hot_region_typed_array_bounds_check_count
hot_region_typed_array_capacity_check_count=$hot_region_typed_array_capacity_check_count
hot_region_typed_array_data_field_load_count=$hot_region_typed_array_data_field_load_count
hot_region_typed_array_direct_data_index_count=$hot_region_typed_array_direct_data_index_count
hot_region_typed_array_per_iter_len_store_count=$hot_region_typed_array_per_iter_len_store_count
hot_region_runtime_array_calls=$hot_region_runtime_array_calls
hot_region_runtime_map_calls=$hot_region_runtime_map_calls
hot_region_runtime_set_calls=$hot_region_runtime_set_calls
hot_region_runtime_property_calls=$hot_region_runtime_property_calls
hot_region_dynamic_dispatch_calls=$hot_region_dynamic_dispatch_calls
hot_region_pending_error_check_count=$hot_region_pending_error_check_count
expect_checked=$expect_checked
audit_pass=$audit_pass
EOF

if $STRICT && [ "$audit_pass" -ne 1 ]; then
    exit 1
fi
exit 0
