/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * typed_drop_cases.inc.c - Typed leaf retirement preserves aliases and charges
 */
static bool mvl_verify_typed_drop_alias(bool negative, bool typed_first) {
    static XrMvlDomain domain;
    xr_mvl_domain_init(&domain);
    XrMvlString *first = xr_mvl_string_new(&domain, "alias", 5), *alias = NULL;
    bool ok = true;
    MVL_REQUIRE(first && xr_mvl_retain(&first->header));
    alias = first;
    /* This immutable leaf fixture establishes a counter band, not publication
     * of a graph or permission to mutate a shared object. */
    if (negative)
        atomic_store_explicit(&first->header.refs, -2, memory_order_release);
    size_t bytes = domain.live_bytes, blocks = domain.live_blocks, allocations = domain.allocations;
    domain.fail_after = 0;
    if (typed_first) xr_mvl_string_drop(first);
    else xr_mvl_drop(&first->header);
    first = NULL;
    MVL_REQUIRE(xr_signed_rc_read(&alias->header.refs) == (negative ? -1 : 1));
    MVL_REQUIRE(domain.live_bytes == bytes && domain.live_blocks == blocks &&
        domain.allocations == allocations && !memcmp(alias->bytes, "alias", 5));
    if (typed_first) xr_mvl_drop(&alias->header);
    else xr_mvl_string_drop(alias);
    alias = NULL;
    MVL_REQUIRE(!domain.live_blocks && !domain.live_bytes && domain.allocations == allocations);
cleanup:
    if (first) xr_mvl_drop(&first->header);
    if (alias) xr_mvl_drop(&alias->header);
    return ok && !domain.live_blocks && !domain.live_bytes;
}

static bool mvl_verify_typed_drop_map_alias(bool negative, bool map_first) {
    static XrMvlDomain domain;
    xr_mvl_domain_init(&domain);
    XrMvlMap *map = xr_mvl_map_new(&domain);
    XrMvlString *key = xr_mvl_string_new(&domain, "stored", 6);
    bool ok = true;
    MVL_REQUIRE(map && key && xr_mvl_map_set(&map, key, 7) == XR_MVL_OK);
    if (negative)
        atomic_store_explicit(&key->header.refs, -2, memory_order_release);
    XrMvlString *stored = key;
    size_t bytes = domain.live_bytes, blocks = domain.live_blocks, allocations = domain.allocations;
    size_t key_bytes = sizeof(*key) + key->length + 1;
    domain.fail_after = 0;
    if (map_first) {
        xr_mvl_drop(&map->header); map = NULL;
        MVL_REQUIRE(domain.live_blocks == 1 && domain.live_bytes == key_bytes);
        MVL_REQUIRE(xr_signed_rc_read(&key->header.refs) == (negative ? -1 : 1));
        xr_mvl_string_drop(key); key = NULL;
    } else {
        xr_mvl_string_drop(key); key = NULL;
        int64_t value = 0;
        MVL_REQUIRE(domain.live_blocks == blocks && domain.live_bytes == bytes &&
            xr_signed_rc_read(&stored->header.refs) == (negative ? -1 : 1));
        MVL_REQUIRE(xr_mvl_map_get(map, stored, &value) && value == 7);
        xr_mvl_drop(&map->header); map = NULL;
    }
    MVL_REQUIRE(!domain.live_blocks && !domain.live_bytes && domain.allocations == allocations);
cleanup:
    if (key) xr_mvl_drop(&key->header);
    if (map) xr_mvl_drop(&map->header);
    return ok && !domain.live_blocks && !domain.live_bytes;
}

static bool mvl_verify_typed_drop_oom(void) {
    static XrMvlDomain domain;
    xr_mvl_domain_init(&domain);
    XrMvlString *string = NULL;
    bool ok = true;
    xr_mvl_string_drop(NULL);
    domain.fail_after = 0;
    string = xr_mvl_string_new(&domain, "oom", 3);
    MVL_REQUIRE(!string && !domain.live_blocks && !domain.live_bytes && !domain.allocations);
    domain.fail_after = 1;
    string = xr_mvl_string_new(&domain, "oom", 3);
    MVL_REQUIRE(string && !domain.fail_after && domain.allocations == 1 &&
        domain.live_blocks == 1 && domain.live_bytes == sizeof(*string) + string->length + 1);
    xr_mvl_string_drop(string); string = NULL;
    MVL_REQUIRE(!domain.live_blocks && !domain.live_bytes && domain.allocations == 1);
cleanup:
    xr_mvl_string_drop(string);
    return ok && !domain.live_blocks && !domain.live_bytes;
}

static bool mvl_verify_typed_drop_failed_take(bool shared, bool negative) {
    static XrMvlDomain domain;
    xr_mvl_domain_init(&domain);
    XrMvlMap *map = xr_mvl_map_new(&domain), *snapshot = NULL;
    XrMvlString *key = NULL;
    bool ok = true;
    MVL_REQUIRE(map);
    for (uint64_t i = 0; i < 6; ++i) {
        char bytes[32];
        size_t length = xr_mvl_key_bytes(i, bytes);
        key = xr_mvl_string_new(&domain, bytes, length);
        MVL_REQUIRE(key && xr_mvl_map_set(&map, key, (int64_t)i) == XR_MVL_OK);
        xr_mvl_drop(&key->header); key = NULL;
    }
    if (shared) {
        MVL_REQUIRE(xr_mvl_retain(&map->header));
        snapshot = map;
    }
    size_t baseline_bytes = domain.live_bytes, baseline_blocks = domain.live_blocks;
    key = xr_mvl_string_new(&domain, "pending", 7);
    MVL_REQUIRE(key);
    if (negative)
        atomic_store_explicit(&key->header.refs, -1, memory_order_release);
    XrMvlMap *before = map;
    XrMvlString *pending = key;
    uint64_t slots = xr_hash_bytes64(map->slots, map->capacity * sizeof(*map->slots));
    size_t bytes = domain.live_bytes, blocks = domain.live_blocks, allocations = domain.allocations;
    domain.fail_after = 0;
    MVL_REQUIRE(xr_mvl_map_set_take(&map, &key, 9) == XR_MVL_OOM && map == before && key == pending);
    MVL_REQUIRE(domain.live_bytes == bytes && domain.live_blocks == blocks && domain.allocations == allocations);
    MVL_REQUIRE(xr_signed_rc_read(&key->header.refs) == (negative ? -1 : 1) &&
        xr_hash_bytes64(map->slots, map->capacity * sizeof(*map->slots)) == slots);
    xr_mvl_string_drop(key); key = NULL;
    MVL_REQUIRE(domain.live_bytes == baseline_bytes && domain.live_blocks == baseline_blocks &&
        domain.allocations == allocations && xr_signed_rc_read(&map->header.refs) == (shared ? 2 : 1));
cleanup:
    xr_mvl_string_drop(key);
    if (map) xr_mvl_drop(&map->header);
    if (snapshot) xr_mvl_drop(&snapshot->header);
    return ok && !domain.live_blocks && !domain.live_bytes;
}

static bool mvl_verify_typed_drop(void) {
    for (unsigned negative = 0; negative < 2; ++negative)
        for (unsigned first = 0; first < 2; ++first)
            if (!mvl_verify_typed_drop_alias(negative != 0, first != 0) ||
                !mvl_verify_typed_drop_map_alias(negative != 0, first != 0) ||
                !mvl_verify_typed_drop_failed_take(first != 0, negative != 0))
                return false;
    return mvl_verify_typed_drop_oom();
}
