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
static bool mvl_verify_direct_attempt(size_t budget, bool shared, bool negative, size_t *sites) {
    static XrMvlDomain domain;
    xr_mvl_domain_init(&domain);
    XrMvlArray *array = xr_mvl_array_new(&domain), *snapshot = NULL;
    bool ok = true;
    MVL_REQUIRE(array && xr_mvl_array_push(&array, 7) == XR_MVL_OK);
    if (shared) {
        MVL_REQUIRE(xr_mvl_retain(&array->header));
        snapshot = array;
        if (negative)
            atomic_store_explicit(&array->header.refs, -2, memory_order_release);
    }
    size_t allocations = domain.allocations, bytes = domain.live_bytes;
    XrMvlArrayWrite write = mvl_array_push_direct_range(array, 0);
    array = write.access.owner;
    MVL_REQUIRE(write.status == XR_MVL_OK && !write.previous && !write.access.writable);
    MVL_REQUIRE(array->length == 1 && domain.allocations == allocations && domain.live_bytes == bytes);
    domain.fail_after = budget;
    write = mvl_array_push_direct_range(array, 9);
    array = write.access.owner;
    MVL_REQUIRE(!write.previous && !write.access.writable);
    if (budget == SIZE_MAX) {
        MVL_REQUIRE(write.status == XR_MVL_OK && array->length == 10);
        *sites = domain.allocations - allocations;
        MVL_REQUIRE(*sites == (shared ? 4u : 2u));
    } else {
        MVL_REQUIRE(write.status == XR_MVL_OOM && array->length >= 1 && array->length < 10);
    }
    MVL_REQUIRE(array->data[0] == 7);
    for (size_t i = 1; i < array->length; ++i)
        MVL_REQUIRE(array->data[i] == (int64_t)i - 1);
    size_t expected_bytes = sizeof(*array) + array->capacity * sizeof(*array->data);
    size_t expected_blocks = 2;
    if (snapshot && snapshot != array) {
        MVL_REQUIRE(snapshot->length == 1 && snapshot->data[0] == 7);
        MVL_REQUIRE(xr_signed_rc_read(&snapshot->header.refs) == (negative ? -1 : 1));
        expected_bytes += sizeof(*snapshot) + snapshot->capacity * sizeof(*snapshot->data);
        expected_blocks += 2;
    } else if (snapshot) {
        MVL_REQUIRE(xr_signed_rc_read(&snapshot->header.refs) == (negative ? -2 : 2));
    }
    MVL_REQUIRE(domain.live_bytes == expected_bytes && domain.live_blocks == expected_blocks);
cleanup:
    if (array) xr_mvl_drop(&array->header);
    if (snapshot) xr_mvl_drop(&snapshot->header);
    return ok && !domain.live_blocks && !domain.live_bytes;
}

static bool mvl_verify_direct(void) {
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        size_t sites = 0;
        bool shared = scenario != 0, negative = scenario == 2;
        if (!mvl_verify_direct_attempt(SIZE_MAX, shared, negative, &sites))
            return false;
        for (size_t budget = 0; budget < sites; ++budget)
            if (!mvl_verify_direct_attempt(budget, shared, negative, &sites))
                return false;
    }
    return true;
}
