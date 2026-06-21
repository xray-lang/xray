#!/bin/bash
# Shared helpers for shell-based Xray tests.

xray_test_file_key() {
    local file="$1"
    local sum size
    if [ -f "$file" ]; then
        set -- $(cksum "$file" 2>/dev/null || printf '0 0')
        sum="${1:-0}"
        size="${2:-0}"
        printf '%s-%s' "$sum" "$size"
    else
        printf 'missing'
    fi
}

xray_test_stable_cache_dir() {
    local project_dir="$1"
    local suite="$2"
    local xray_bin="$3"
    local root key

    root="${XRAY_TEST_CACHE_ROOT:-$project_dir/build/.xray-test-cache}"
    key="$(xray_test_file_key "$xray_bin")"
    printf '%s/%s/%s' "$root" "$suite" "$key"
}

xray_test_case_dir_key() {
    local case_file="$1"
    local dir f
    dir="$(dirname "$case_file")"
    (
        for f in "$dir"/*.xr "$dir"/*.args; do
            [ -f "$f" ] || continue
            printf '%s ' "$(basename "$f")"
            cksum "$f" 2>/dev/null || printf '0 0 %s\n' "$f"
        done
    ) | cksum | awk '{ print $1 "-" $2 }'
}
