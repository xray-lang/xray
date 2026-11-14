/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * domain_cases.inc.c - Object descriptors preserve independent billing owners
 */
static bool mvl_verify_domains(bool take, bool negative) {
    static XrMvlDomain maps, keys;
    xr_mvl_domain_init(&maps);
    xr_mvl_domain_init(&keys);
    XrMvlMap *map = xr_mvl_map_new(&maps), *snapshot = NULL;
    XrMvlString *seed = xr_mvl_string_new(&keys, "seed", 4);
    XrMvlString *pending = NULL, *duplicate = NULL;
    bool ok = true;
    MVL_REQUIRE(map && seed && xr_mvl_map_set_take(&map, &seed, 3) == XR_MVL_OK && !seed);
    MVL_REQUIRE(map->header.ops == &maps.map_ops && maps.map_ops.domain == &maps);
    MVL_REQUIRE(maps.live_blocks == 2 &&
        maps.live_bytes == sizeof(*map) + map->capacity * sizeof(*map->slots));
    MVL_REQUIRE(keys.live_blocks == 1 && keys.live_bytes == sizeof(XrMvlString) + 5);
    MVL_REQUIRE(xr_mvl_retain(&map->header));
    snapshot = map;
    pending = xr_mvl_string_new(&keys, "next", 4);
    MVL_REQUIRE(pending && pending->header.ops == &keys.string_ops);
    if (negative)
        atomic_store_explicit(&pending->header.refs, -1, memory_order_release);
    size_t map_bytes = maps.live_bytes, map_blocks = maps.live_blocks;
    size_t key_bytes = keys.live_bytes, key_blocks = keys.live_blocks;
    size_t key_allocations = keys.allocations;
    keys.fail_after = 0;
    for (size_t budget = 0; budget < 2; ++budget) {
        maps.fail_after = budget;
        size_t allocations = maps.allocations;
        XrMvlString *before = pending;
        XrMvlStatus status = take ? xr_mvl_map_set_take(&map, &pending, 7) :
            xr_mvl_map_set(&map, pending, 7);
        MVL_REQUIRE(status == XR_MVL_OOM && map == snapshot && pending == before);
        MVL_REQUIRE(maps.live_bytes == map_bytes && maps.live_blocks == map_blocks &&
            maps.allocations == allocations + budget && !maps.fail_after);
        MVL_REQUIRE(keys.live_bytes == key_bytes && keys.live_blocks == key_blocks &&
            keys.allocations == key_allocations && !keys.fail_after);
        MVL_REQUIRE(xr_signed_rc_read(&pending->header.refs) == (negative ? -1 : 1));
    }
    maps.fail_after = SIZE_MAX;
    XrMvlString *stored = pending;
    MVL_REQUIRE((take ? xr_mvl_map_set_take(&map, &pending, 7) :
        xr_mvl_map_set(&map, pending, 7)) == XR_MVL_OK);
    MVL_REQUIRE(map != snapshot && map->header.ops == &maps.map_ops &&
        stored->header.ops == &keys.string_ops && (take ? !pending : pending == stored));
    if (pending) { xr_mvl_drop(&pending->header); pending = NULL; }
    int64_t value;
    MVL_REQUIRE(xr_mvl_map_get(map, stored, &value) && value == 7 &&
        !xr_mvl_map_get(snapshot, stored, &value));
    /* Equal-key take retires the incoming object in its own allocation domain,
     * while the stored equal key remains charged to the other domain. */
    size_t current_map_bytes = maps.live_bytes;
    duplicate = xr_mvl_string_new(&maps, "next", 4);
    MVL_REQUIRE(duplicate && xr_mvl_map_set_take(&map, &duplicate, 9) == XR_MVL_OK && !duplicate);
    MVL_REQUIRE(maps.live_bytes == current_map_bytes && keys.live_bytes == key_bytes);
    xr_mvl_drop(&snapshot->header); snapshot = NULL;
    MVL_REQUIRE(keys.live_blocks == 2 && keys.live_bytes == key_bytes);
    xr_mvl_drop(&map->header); map = NULL;
    MVL_REQUIRE(!maps.live_blocks && !maps.live_bytes && !keys.live_blocks && !keys.live_bytes);
cleanup:
    if (seed) xr_mvl_drop(&seed->header);
    if (pending) xr_mvl_drop(&pending->header);
    if (duplicate) xr_mvl_drop(&duplicate->header);
    if (snapshot) xr_mvl_drop(&snapshot->header);
    if (map) xr_mvl_drop(&map->header);
    return ok && !maps.live_blocks && !maps.live_bytes && !keys.live_blocks && !keys.live_bytes;
}

static bool mvl_verify_string_extent(void) {
    static XrMvlDomain domain;
    xr_mvl_domain_init(&domain);
    bool ok = true;
    XrMvlString *empty = NULL;
    MVL_REQUIRE(!xr_mvl_string_new(&domain, "x", SIZE_MAX) && !domain.allocations &&
        !domain.live_bytes && !domain.live_blocks && domain.fail_after == SIZE_MAX);
    MVL_REQUIRE(!xr_mvl_string_new(&domain, "x", SIZE_MAX - sizeof(XrMvlString)) &&
        !domain.allocations && !domain.live_bytes && !domain.live_blocks);
    empty = xr_mvl_string_new(&domain, NULL, 0);
    MVL_REQUIRE(empty && !empty->length && !empty->bytes[0] && domain.live_blocks == 1 &&
        domain.live_bytes == sizeof(*empty) + 1 && empty->header.ops == &domain.string_ops);
cleanup:
    if (empty) xr_mvl_drop(&empty->header);
    return ok && !domain.live_blocks && !domain.live_bytes;
}
