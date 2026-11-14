/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * verify.c - Explicit ownership and failed-preparation state checks
 *
 * KEY CONCEPT:
 *   Failure is checked against real bindings, references and allocation charge.
 */
#include "access.h"
#include "direct.inc.c"

#define MVL_REQUIRE(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "state check failed at line %d: %s\n", __LINE__, #condition); \
        ok = false; \
        goto cleanup; \
    } \
} while (0)

static bool mvl_verify_array(void) {
    static XrMvlDomain domain;
    xr_mvl_domain_init(&domain);
    XrMvlArray *array = xr_mvl_array_new(&domain), *snapshot = NULL;
    bool ok = true;
    MVL_REQUIRE(array);
    for (int64_t i = 0; i < 4; ++i)
        MVL_REQUIRE(xr_mvl_array_push(&array, i) == XR_MVL_OK);
    XrMvlArray *before = array;
    size_t bytes = domain.live_bytes, blocks = domain.live_blocks;
    domain.fail_after = 0;
    MVL_REQUIRE(xr_mvl_array_push(&array, 4) == XR_MVL_OOM);
    MVL_REQUIRE(array == before && array->length == 4 && array->capacity == 4);
    MVL_REQUIRE(domain.live_bytes == bytes && domain.live_blocks == blocks);
    MVL_REQUIRE(xr_mvl_array_set(&array, 4, 99) == XR_MVL_BOUNDS);
    MVL_REQUIRE(xr_mvl_retain(&array->header));
    snapshot = array;
    for (size_t budget = 0; budget < 2; ++budget) {
        domain.fail_after = budget;
        MVL_REQUIRE(xr_mvl_array_push(&array, 4) == XR_MVL_OOM);
        MVL_REQUIRE(array == before && xr_signed_rc_read(&snapshot->header.refs) == 2);
        MVL_REQUIRE(domain.live_bytes == bytes && domain.live_blocks == blocks);
    }
    domain.fail_after = SIZE_MAX;
    MVL_REQUIRE(xr_mvl_array_push(&array, 4) == XR_MVL_OK);
    MVL_REQUIRE(array != snapshot && array->length == 5 && snapshot->length == 4);
    MVL_REQUIRE(array->data[4] == 4 && snapshot->data[3] == 3 &&
        xr_signed_rc_read(&snapshot->header.refs) == 1);
    xr_mvl_drop(&snapshot->header);
    snapshot = NULL;
    MVL_REQUIRE(xr_mvl_retain(&array->header));
    snapshot = array;
    bytes = domain.live_bytes;
    domain.fail_after = 1;
    MVL_REQUIRE(xr_mvl_array_set(&array, 0, 99) == XR_MVL_OOM);
    MVL_REQUIRE(array == snapshot && array->data[0] == 0 && domain.live_bytes == bytes);
    domain.fail_after = SIZE_MAX;
    MVL_REQUIRE(xr_mvl_array_set(&array, 0, 99) == XR_MVL_OK);
    MVL_REQUIRE(array != snapshot && array->data[0] == 99 && snapshot->data[0] == 0);
cleanup:
    if (array)
        xr_mvl_drop(&array->header);
    if (snapshot)
        xr_mvl_drop(&snapshot->header);
    return ok && domain.live_blocks == 0 && domain.live_bytes == 0;
}

static bool mvl_verify_map(void) {
    static XrMvlDomain domain;
    xr_mvl_domain_init(&domain);
    XrMvlMap *map = xr_mvl_map_new(&domain), *snapshot = NULL;
    XrMvlString *seed = xr_mvl_string_new(&domain, "seed", 4);
    const char embedded[] = {'k', '\0', '1'};
    XrMvlString *key = xr_mvl_string_new(&domain, embedded, sizeof(embedded));
    bool ok = true;
    MVL_REQUIRE(map && seed && key);
    MVL_REQUIRE(xr_mvl_map_set(&map, seed, 7) == XR_MVL_OK);
    MVL_REQUIRE(xr_signed_rc_read(&seed->header.refs) == 2 && xr_mvl_retain(&map->header));
    snapshot = map;
    size_t bytes = domain.live_bytes, blocks = domain.live_blocks;
    for (size_t budget = 0; budget < 2; ++budget) {
        domain.fail_after = budget;
        MVL_REQUIRE(xr_mvl_map_set(&map, key, 11) == XR_MVL_OOM);
        MVL_REQUIRE(map == snapshot && map->length == 1 && xr_signed_rc_read(&map->header.refs) == 2);
        MVL_REQUIRE(xr_signed_rc_read(&key->header.refs) == 1 &&
            xr_signed_rc_read(&seed->header.refs) == 2);
        MVL_REQUIRE(domain.live_bytes == bytes && domain.live_blocks == blocks);
    }
    domain.fail_after = SIZE_MAX;
    MVL_REQUIRE(xr_mvl_map_set(&map, key, 11) == XR_MVL_OK);
    int64_t value;
    MVL_REQUIRE(map != snapshot && xr_signed_rc_read(&key->header.refs) == 2 &&
        xr_signed_rc_read(&seed->header.refs) == 3);
    MVL_REQUIRE(!xr_mvl_map_get(snapshot, key, &value));
    MVL_REQUIRE(xr_mvl_map_get(map, key, &value) && value == 11);
    domain.fail_after = 0;
    MVL_REQUIRE(xr_mvl_map_set(&map, seed, 19) == XR_MVL_OK);
    MVL_REQUIRE(xr_mvl_map_get(snapshot, seed, &value) && value == 7);
    MVL_REQUIRE(xr_mvl_map_get(map, seed, &value) && value == 19);
cleanup:
    if (map)
        xr_mvl_drop(&map->header);
    if (snapshot)
        xr_mvl_drop(&snapshot->header);
    if (seed)
        xr_mvl_drop(&seed->header);
    if (key)
        xr_mvl_drop(&key->header);
    return ok && domain.live_blocks == 0 && domain.live_bytes == 0;
}

static bool mvl_verify_record(void) {
    static XrMvlDomain domain;
    xr_mvl_domain_init(&domain);
    XrMvlPairObject *first = xr_mvl_pair_new(&domain, (XrMvlPair) {1, 2});
    XrMvlPairObject *second = xr_mvl_pair_new(&domain, (XrMvlPair) {3, 4});
    bool ok = true;
    MVL_REQUIRE(first && second);
    MVL_REQUIRE(xr_mvl_pair_escape(first) && xr_signed_rc_read(&first->header.refs) == 2);
    MVL_REQUIRE(xr_mvl_pair_escaped() == first);
    MVL_REQUIRE(xr_mvl_pair_escape(second));
    MVL_REQUIRE(xr_signed_rc_read(&first->header.refs) == 1 && first->value.y == 2);
    MVL_REQUIRE(xr_signed_rc_read(&second->header.refs) == 2 && xr_mvl_pair_escaped()->value.x == 3);
    atomic_store_explicit(&first->header.refs, INT32_MAX, memory_order_relaxed);
    bool retained = xr_mvl_pair_escape(first);
    atomic_store_explicit(&first->header.refs, 1, memory_order_relaxed);
    MVL_REQUIRE(!retained && xr_mvl_pair_escaped() == second);
cleanup:
    xr_mvl_pair_escape_clear();
    if (first)
        xr_mvl_drop(&first->header);
    if (second)
        xr_mvl_drop(&second->header);
    return ok && domain.live_blocks == 0 && domain.live_bytes == 0;
}

static bool mvl_verify_map_growth(void) {
    static XrMvlDomain domain;
    xr_mvl_domain_init(&domain);
    XrMvlMap *map = xr_mvl_map_new(&domain);
    XrMvlString *key = NULL;
    bool ok = true;
    MVL_REQUIRE(map);
    for (uint64_t i = 0; i < 6; ++i) {
        char bytes[32];
        size_t length = xr_mvl_key_bytes(i, bytes);
        key = xr_mvl_string_new(&domain, bytes, length);
        MVL_REQUIRE(key && xr_mvl_map_set(&map, key, (int64_t) i) == XR_MVL_OK);
        xr_mvl_drop(&key->header);
        key = NULL;
    }
    key = xr_mvl_string_new(&domain, "next", 4);
    MVL_REQUIRE(key);
    XrMvlMapSlot *slots = map->slots;
    size_t bytes = domain.live_bytes, blocks = domain.live_blocks;
    domain.fail_after = 0;
    MVL_REQUIRE(xr_mvl_map_set(&map, key, 6) == XR_MVL_OOM);
    MVL_REQUIRE(map->slots == slots && map->capacity == 8 && map->length == 6);
    MVL_REQUIRE(domain.live_bytes == bytes && domain.live_blocks == blocks);
    MVL_REQUIRE(xr_signed_rc_read(&key->header.refs) == 1);
    domain.fail_after = SIZE_MAX;
    MVL_REQUIRE(xr_mvl_map_set(&map, key, 6) == XR_MVL_OK);
    MVL_REQUIRE(map->capacity == 16 && map->length == 7 && xr_signed_rc_read(&key->header.refs) == 2);
cleanup:
    if (key)
        xr_mvl_drop(&key->header);
    if (map)
        xr_mvl_drop(&map->header);
    return ok && domain.live_blocks == 0 && domain.live_bytes == 0;
}

static bool mvl_verify_header_counter(void) {
    static XrMvlDomain domain;
    xr_mvl_domain_init(&domain);
    XrMvlString *string = xr_mvl_string_new(&domain, "counter", 7);
    bool ok = true;
    MVL_REQUIRE(string);
    /* This leaf has no child graph. The test establishes a shared counter,
     * not a public graph-publication or cross-execution entry point. */
    atomic_store_explicit(&string->header.refs, -1, memory_order_release);
    MVL_REQUIRE(xr_mvl_retain(&string->header));
    MVL_REQUIRE(xr_signed_rc_read(&string->header.refs) == -2);
    MVL_REQUIRE(!xr_signed_rc_unique(&string->header.refs));
    xr_mvl_drop(&string->header);
    MVL_REQUIRE(xr_signed_rc_read(&string->header.refs) == -1);
    MVL_REQUIRE(xr_signed_rc_unique(&string->header.refs));
    MVL_REQUIRE(xr_signed_rc_read(&string->header.refs) == 1);
cleanup:
    if (string)
        xr_mvl_drop(&string->header);
    return ok && domain.live_blocks == 0 && domain.live_bytes == 0;
}

#include "take_cases.inc.c"
#include "access_cases.inc.c"
#include "direct_cases.inc.c"

XR_FUNCDEF bool xr_mvl_verify(void) {
    size_t capacity;
    if (xr_mvl_capacity(0, SIZE_MAX, sizeof(int64_t), &capacity))
        return false;
    return mvl_verify_array() && mvl_verify_map() && mvl_verify_map_growth() &&
        mvl_verify_record() && mvl_verify_header_counter() && mvl_verify_take() &&
        mvl_verify_access() && mvl_verify_direct();
}
