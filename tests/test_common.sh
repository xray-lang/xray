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

xray_test_string_key() {
    printf '%s' "$1" | cksum | awk '{ print $1 "-" $2 }'
}

xray_test_tree_key() {
    local dir="$1"
    local rel f
    if [ ! -d "$dir" ]; then
        printf 'missing'
        return 0
    fi
    (
        find "$dir" -type f \( -name '*.h' -o -name '*.inc.c' \) 2>/dev/null |
            LC_ALL=C sort |
            while IFS= read -r f; do
                rel="${f#$dir/}"
                printf '%s ' "$rel"
                cksum "$f" 2>/dev/null || printf '0 0 %s\n' "$f"
            done
    ) | cksum | awk '{ print $1 "-" $2 }'
}

xray_test_toolchain_key() {
    local xray_bin="$1"
    local bin_dir project_dir

    bin_dir="$(cd "$(dirname "$xray_bin")" 2>/dev/null && pwd || printf '.')"
    project_dir="$(cd "$bin_dir/.." 2>/dev/null && pwd || printf '')"
    (
        printf 'xray-test-toolchain-cache-schema 2\n'
        printf 'xray %s\n' "$(xray_test_file_key "$xray_bin")"
        printf 'libxray_aot_core.a %s\n' "$(xray_test_file_key "$bin_dir/libxray_aot_core.a")"
        printf 'libxray_rt_coro.a %s\n' "$(xray_test_file_key "$bin_dir/libxray_rt_coro.a")"
        printf 'libxray_core.a %s\n' "$(xray_test_file_key "$bin_dir/libxray_core.a")"
        printf 'src/aot headers %s\n' "$(xray_test_tree_key "$project_dir/src/aot")"
        printf 'src/shared headers %s\n' "$(xray_test_tree_key "$project_dir/src/shared")"
        printf 'src/coro headers %s\n' "$(xray_test_tree_key "$project_dir/src/coro")"
    ) | cksum | awk '{ print $1 "-" $2 }'
}

xray_test_stable_cache_dir() {
    local project_dir="$1"
    local suite="$2"
    local xray_bin="$3"
    local root key

    root="${XRAY_TEST_CACHE_ROOT:-$project_dir/build/.xray-test-cache}"
    key="$(xray_test_toolchain_key "$xray_bin")"
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
    local dir f cache_var cached key
    dir="$(dirname "$case_file")"
    cache_var="XRAY_TEST_CASE_DIR_KEY_$(printf '%s' "$dir" | cksum | awk '{ print $1 "_" $2 }')"
    eval "cached=\${$cache_var:-}"
    if [ -n "$cached" ]; then
        printf '%s\n' "$cached"
        return 0
    fi
    key="$((
        for f in "$dir"/*.xr "$dir"/*.args "$dir"/*.stdin; do
            [ -f "$f" ] || continue
            printf '%s ' "$(basename "$f")"
            cksum "$f" 2>/dev/null || printf '0 0 %s\n' "$f"
        done
    ) | cksum | awk '{ print $1 "-" $2 }')"
    eval "$cache_var=\$key"
    printf '%s\n' "$key"
}

xray_test_lock_dir() {
    local lock_dir="$1"
    local waited=0
    local timeout="${XRAY_TEST_LOCK_TIMEOUT:-3000}"

    mkdir -p "$(dirname "$lock_dir")" 2>/dev/null || return 1

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
