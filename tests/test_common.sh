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

xray_test_shared_cache_dir() {
    local project_dir="$1"
    local suite="$2"
    local root

    root="${XRAY_TEST_CACHE_ROOT:-$project_dir/build/.xray-test-cache}"
    printf '%s/%s' "$root" "$suite"
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

xray_test_lock_dir() {
    local lock_dir="$1"
    local waited=0
    local timeout="${XRAY_TEST_LOCK_TIMEOUT:-3000}"

    while ! mkdir "$lock_dir" 2>/dev/null; do
        if [ -d "$lock_dir" ]; then
            if [ "$waited" -ge "$timeout" ] 2>/dev/null; then
                return 1
            fi
            waited=$((waited + 1))
            sleep 0.1
            continue
        fi
        return 1
    done
    return 0
}

xray_test_unlock_dir() {
    rmdir "$1" 2>/dev/null || true
}
