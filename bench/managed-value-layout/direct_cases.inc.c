/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * direct_cases.inc.c - Committed-prefix and owner accounting for the direct loop
 *
 * KEY CONCEPT:
 *   Exercise the actual loop, not a mirror of its writes; failed preparation
 *   preserves already committed elements and refunds all temporary allocations.
 */
typedef XrMvlArrayWrite (*MvlRangeWrite)(XrMvlArray, int64_t);

static bool mvl_verify_direct_attempt(MvlRangeWrite range, size_t budget,
    bool shared, bool negative, size_t *sites) {
    static XrMvlDomain domain;
    xr_mvl_domain_init(&domain);
    XrMvlArray array = xr_mvl_array_new(&domain), snapshot = {0};
    bool ok = true;
    MVL_REQUIRE(array.owner && xr_mvl_array_push(&array, 7) == XR_MVL_OK);
    if (shared) {
        MVL_REQUIRE(xr_mvl_retain(&array.owner->header));
        snapshot = array;
        if (negative)
            atomic_store_explicit(&array.owner->header.refs, -2, memory_order_release);
    }
    size_t allocations = domain.allocations, bytes = domain.live_bytes;
    XrMvlArrayWrite write = range(array, 0);
    array = write.access.value;
    MVL_REQUIRE(write.status == XR_MVL_OK && !write.previous && !write.access.writable);
    MVL_REQUIRE(array.length == 1 && domain.allocations == allocations && domain.live_bytes == bytes);
    domain.fail_after = budget;
    write = range(array, 9);
    array = write.access.value;
    MVL_REQUIRE(!write.previous && !write.access.writable);
    if (budget == SIZE_MAX) {
        MVL_REQUIRE(write.status == XR_MVL_OK && array.length == 10);
        *sites = domain.allocations - allocations;
        MVL_REQUIRE(*sites == (shared ? 3u : 2u));
    } else {
        size_t cow_allocations = *sites - 2;
        size_t committed = budget < cow_allocations ? 1 : budget == cow_allocations ? 4 : 8;
        MVL_REQUIRE(write.status == XR_MVL_OOM && array.length == committed);
    }
    MVL_REQUIRE(array.owner->data[0] == 7);
    for (size_t i = 1; i < array.length; ++i)
        MVL_REQUIRE(array.owner->data[i] == (int64_t)i - 1);
    size_t expected_bytes = sizeof(*array.owner) + array.owner->capacity * sizeof(*array.owner->data);
    size_t expected_blocks = 1;
    if (snapshot.owner && snapshot.owner != array.owner) {
        MVL_REQUIRE(snapshot.length == 1 && snapshot.owner->data[0] == 7);
        MVL_REQUIRE(xr_signed_rc_read(&snapshot.owner->header.refs) == (negative ? -1 : 1));
        expected_bytes += sizeof(*snapshot.owner) + snapshot.owner->capacity * sizeof(*snapshot.owner->data);
        ++expected_blocks;
    } else if (snapshot.owner) {
        MVL_REQUIRE(xr_signed_rc_read(&snapshot.owner->header.refs) == (negative ? -2 : 2));
    }
    MVL_REQUIRE(domain.live_bytes == expected_bytes && domain.live_blocks == expected_blocks);
cleanup:
    if (array.owner) xr_mvl_drop(&array.owner->header);
    if (snapshot.owner) xr_mvl_drop(&snapshot.owner->header);
    return ok && !domain.live_blocks && !domain.live_bytes;
}

static bool mvl_verify_range(MvlRangeWrite range) {
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        size_t sites = 0;
        bool shared = scenario != 0, negative = scenario == 2;
        if (!mvl_verify_direct_attempt(range, SIZE_MAX, shared, negative, &sites))
            return false;
        for (size_t budget = 0; budget < sites; ++budget)
            if (!mvl_verify_direct_attempt(range, budget, shared, negative, &sites))
                return false;
    }
    return true;
}

static bool mvl_verify_direct(void) {
    return mvl_verify_range(mvl_array_push_direct_range) &&
        mvl_verify_range(mvl_array_push_noalias_range);
}
