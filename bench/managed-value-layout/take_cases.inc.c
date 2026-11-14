/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * take_cases.inc.c - Real-owner transfer and complete preparation failure checks
 *
 * KEY CONCEPT:
 *   A transferred reference may have aliases; count alone grants no permission.
 */
static bool mvl_verify_take_alias(bool negative) {
    static XrMvlDomain domain;
    xr_mvl_domain_init(&domain);
    XrMvlMap *map = xr_mvl_map_new(&domain), *snapshot = NULL;
    XrMvlString *key = xr_mvl_string_new(&domain, "alias", 5);
    XrMvlString *alias = NULL, *duplicate = NULL;
    bool ok = true;
    MVL_REQUIRE(map && key && xr_mvl_retain(&key->header));
    alias = key;
    /* Controlled counter fixtures do not publish a graph to another thread. */
    if (negative)
        atomic_store_explicit(&key->header.refs, -2, memory_order_release);
    int32_t count = negative ? -2 : 2;
    MVL_REQUIRE(xr_mvl_map_set_take(&map, &key, 1) == XR_MVL_OK && !key);
    MVL_REQUIRE(xr_signed_rc_read(&alias->header.refs) == count);
    size_t slot = xr_mvl_map_probe(map, alias);
    MVL_REQUIRE(map->slots[slot].key == alias && map->length == 1);
    duplicate = xr_mvl_string_new(&domain, "alias", 5);
    MVL_REQUIRE(duplicate);
    if (negative)
        atomic_store_explicit(&duplicate->header.refs, -1, memory_order_release);
    size_t bytes = domain.live_bytes, blocks = domain.live_blocks;
    size_t duplicate_bytes = duplicate->header.allocation_bytes;
    MVL_REQUIRE(xr_mvl_map_set_take(&map, &duplicate, 2) == XR_MVL_OK && !duplicate);
    MVL_REQUIRE(domain.live_bytes == bytes - duplicate_bytes && domain.live_blocks == blocks - 1);
    MVL_REQUIRE(map->slots[slot].key == alias && map->slots[slot].value == 2);
    MVL_REQUIRE(xr_signed_rc_read(&alias->header.refs) == count);
    MVL_REQUIRE(xr_mvl_retain(&map->header));
    snapshot = map;
    if (negative)
        atomic_store_explicit(&map->header.refs, -2, memory_order_release);
    XrMvlString *stored = alias;
    MVL_REQUIRE(xr_mvl_map_set_take(&map, &alias, 3) == XR_MVL_OK && !alias);
    MVL_REQUIRE(map != snapshot && snapshot->slots[slot].value == 2);
    slot = xr_mvl_map_probe(map, stored);
    MVL_REQUIRE(map->slots[slot].key == stored && map->slots[slot].value == 3);
    MVL_REQUIRE(xr_signed_rc_read(&stored->header.refs) == count);
    xr_mvl_drop(&snapshot->header);
    snapshot = NULL;
    MVL_REQUIRE(xr_mvl_retain(&stored->header));
    alias = stored;
    MVL_REQUIRE(xr_mvl_map_set_take(&map, &alias, 4) == XR_MVL_OK && !alias);
    MVL_REQUIRE(map->slots[slot].value == 4);
    MVL_REQUIRE(xr_signed_rc_read(&stored->header.refs) == (negative ? -1 : 1));
    MVL_REQUIRE(xr_mvl_map_set_take(&map, &key, 4) == XR_MVL_INVALID_OWNER);
cleanup:
    if (key) xr_mvl_drop(&key->header);
    if (alias) xr_mvl_drop(&alias->header);
    if (duplicate) xr_mvl_drop(&duplicate->header);
    if (map) xr_mvl_drop(&map->header);
    if (snapshot) xr_mvl_drop(&snapshot->header);
    return ok && !domain.live_blocks && !domain.live_bytes;
}

static bool mvl_verify_take_attempt(size_t budget, unsigned scenario, size_t *sites) {
    static XrMvlDomain domain;
    xr_mvl_domain_init(&domain);
    bool shared = (scenario & 1) != 0;
    bool duplicate = (scenario & 2) != 0;
    bool negative = (scenario & 4) != 0;
    XrMvlMap *map = xr_mvl_map_new(&domain), *snapshot = NULL;
    XrMvlString *key = NULL, *original_keys[6] = {0};
    bool ok = true;
    MVL_REQUIRE(map);
    for (uint64_t i = 0; i < 6; ++i) {
        char bytes[32];
        size_t length = xr_mvl_key_bytes(i, bytes);
        key = xr_mvl_string_new(&domain, bytes, length);
        MVL_REQUIRE(key && xr_mvl_map_set(&map, key, (int64_t) i) == XR_MVL_OK);
        original_keys[i] = key;
        xr_mvl_drop(&key->header);
        key = NULL;
        if (negative)
            atomic_store_explicit(&original_keys[i]->header.refs, -1, memory_order_release);
    }
    if (shared) {
        MVL_REQUIRE(xr_mvl_retain(&map->header));
        snapshot = map;
        if (negative)
            atomic_store_explicit(&map->header.refs, -2, memory_order_release);
    }
    key = xr_mvl_string_new(&domain, duplicate ? "k0" : "new", duplicate ? 2 : 3);
    MVL_REQUIRE(key);
    if (negative)
        atomic_store_explicit(&key->header.refs, -1, memory_order_release);
    XrMvlMap *before = map;
    XrMvlString *before_key = key;
    XrMvlMapSlot *before_slots = map->slots;
    uint64_t before_hash = xr_hash_bytes64(map->slots, map->capacity * sizeof(*map->slots));
    size_t bytes = domain.live_bytes, blocks = domain.live_blocks, allocations = domain.allocations;
    domain.fail_after = budget;
    XrMvlStatus status = xr_mvl_map_set_take(&map, &key, 99);
    if (budget == SIZE_MAX || (!shared && duplicate)) {
        MVL_REQUIRE(status == XR_MVL_OK && !key);
        *sites = domain.allocations - allocations;
        MVL_REQUIRE(*sites == (shared ? 2u : duplicate ? 0u : 1u));
        MVL_REQUIRE(map->length == (duplicate ? 6u : 7u));
        int64_t value;
        XrMvlString *query = duplicate ? original_keys[0] : before_key;
        MVL_REQUIRE(xr_mvl_map_get(map, query, &value) && value == 99);
        MVL_REQUIRE(map->slots[xr_mvl_map_probe(map, query)].key == query);
        if (snapshot) {
            MVL_REQUIRE(map != snapshot && snapshot->length == 6);
            MVL_REQUIRE(xr_hash_bytes64(snapshot->slots, snapshot->capacity * sizeof(*snapshot->slots)) == before_hash);
            MVL_REQUIRE(xr_signed_rc_read(&snapshot->header.refs) == (negative ? -1 : 1));
        }
    } else {
        MVL_REQUIRE(status == XR_MVL_OOM && map == before && key == before_key);
        MVL_REQUIRE(map->slots == before_slots && map->capacity == 8 && map->length == 6);
        MVL_REQUIRE(xr_hash_bytes64(map->slots, map->capacity * sizeof(*map->slots)) == before_hash);
        MVL_REQUIRE(domain.live_bytes == bytes && domain.live_blocks == blocks);
        MVL_REQUIRE(xr_signed_rc_read(&key->header.refs) == (negative ? -1 : 1));
        MVL_REQUIRE(xr_signed_rc_read(&map->header.refs) == (shared ? negative ? -2 : 2 : 1));
        for (size_t i = 0; i < 6; ++i)
            MVL_REQUIRE(xr_signed_rc_read(&original_keys[i]->header.refs) == (negative ? -1 : 1));
    }
cleanup:
    if (key) xr_mvl_drop(&key->header);
    if (map) xr_mvl_drop(&map->header);
    if (snapshot) xr_mvl_drop(&snapshot->header);
    return ok && !domain.live_blocks && !domain.live_bytes;
}

static bool mvl_verify_take_retain_limit(bool negative) {
    static XrMvlDomain domain;
    xr_mvl_domain_init(&domain);
    XrMvlMap *map = xr_mvl_map_new(&domain), *snapshot = NULL;
    XrMvlString *key = NULL, *limit = NULL;
    bool ok = true;
    MVL_REQUIRE(map);
    for (uint64_t i = 0; i < 3; ++i) {
        char bytes[32];
        size_t length = xr_mvl_key_bytes(i, bytes);
        key = xr_mvl_string_new(&domain, bytes, length);
        MVL_REQUIRE(key && xr_mvl_map_set_take(&map, &key, (int64_t) i) == XR_MVL_OK && !key);
    }
    for (size_t i = 0; i < map->capacity; ++i)
        if (map->slots[i].key) {
            limit = map->slots[i].key;
            if (negative)
                atomic_store_explicit(&limit->header.refs, -1, memory_order_release);
        }
    MVL_REQUIRE(limit && xr_mvl_retain(&map->header));
    snapshot = map;
    if (negative)
        atomic_store_explicit(&map->header.refs, -2, memory_order_release);
    key = xr_mvl_string_new(&domain, "limit", 5);
    MVL_REQUIRE(key);
    if (negative)
        atomic_store_explicit(&key->header.refs, -1, memory_order_release);
    XrMvlString *before = key;
    size_t bytes = domain.live_bytes, blocks = domain.live_blocks;
    atomic_store_explicit(&limit->header.refs, negative ? -INT32_MAX : INT32_MAX, memory_order_relaxed);
    XrMvlStatus status = xr_mvl_map_set_take(&map, &key, 4);
    atomic_store_explicit(&limit->header.refs, negative ? -1 : 1, memory_order_relaxed);
    MVL_REQUIRE(status == XR_MVL_RC_OVERFLOW && map == snapshot && key == before);
    MVL_REQUIRE(domain.live_bytes == bytes && domain.live_blocks == blocks);
    MVL_REQUIRE(xr_signed_rc_read(&key->header.refs) == (negative ? -1 : 1));
    for (size_t i = 0; i < map->capacity; ++i)
        if (map->slots[i].key)
            MVL_REQUIRE(xr_signed_rc_read(&map->slots[i].key->header.refs) == (negative ? -1 : 1));
cleanup:
    if (key) xr_mvl_drop(&key->header);
    if (map) xr_mvl_drop(&map->header);
    if (snapshot) xr_mvl_drop(&snapshot->header);
    return ok && !domain.live_blocks && !domain.live_bytes;
}

static bool mvl_verify_take(void) {
    if (!mvl_verify_take_alias(false) || !mvl_verify_take_alias(true))
        return false;
    const unsigned scenarios[] = {0, 2, 1, 3, 5, 7};
    for (size_t i = 0; i < sizeof(scenarios) / sizeof(*scenarios); ++i) {
        size_t sites = 0;
        if (!mvl_verify_take_attempt(SIZE_MAX, scenarios[i], &sites))
            return false;
        for (size_t budget = 0; budget < sites; ++budget)
            if (!mvl_verify_take_attempt(budget, scenarios[i], &sites))
                return false;
        if (sites == 0 && !mvl_verify_take_attempt(0, scenarios[i], &sites))
            return false;
    }
    return mvl_verify_take_retain_limit(false) && mvl_verify_take_retain_limit(true);
}
