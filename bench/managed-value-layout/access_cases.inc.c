/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * access_cases.inc.c - Prepared access lifetime and preparation state checks
 *
 * KEY CONCEPT:
 *   Caller publication is separate from cold preparation; a cached span owns no
 *   reference and is closed before operations that invalidate its authority.
 */
static void mvl_access_commit(XrMvlArray **binding, XrMvlArrayAccess *access, XrMvlArrayWrite write) {
    *binding = write.access.owner;
    *access = write.access;
    if (write.previous)
        xr_mvl_drop(&write.previous->header);
}

static bool mvl_verify_access_cow(size_t budget, bool negative, bool abort_write, size_t *sites) {
    static XrMvlDomain domain;
    xr_mvl_domain_init(&domain);
    XrMvlArray *array = xr_mvl_array_new(&domain), *snapshot = NULL;
    XrMvlArrayWrite write = {0};
    bool pending = false, ok = true;
    MVL_REQUIRE(array && xr_mvl_array_push(&array, 7) == XR_MVL_OK);
    MVL_REQUIRE(xr_mvl_retain(&array->header));
    snapshot = array;
    if (negative)
        atomic_store_explicit(&array->header.refs, -2, memory_order_release);
    size_t bytes = domain.live_bytes, blocks = domain.live_blocks, allocations = domain.allocations;
    XrMvlArrayAccess access = xr_mvl_array_access_start(array);
    MVL_REQUIRE(!access.writable && xr_signed_rc_read(&array->header.refs) == (negative ? -2 : 2));
    /* An empty iteration range never prepares a write or clones a shared body. */
    size_t iterations = 0;
    for (size_t i = 0; i < iterations; ++i)
        write = xr_mvl_array_access_write(access, 0, 8, true);
    MVL_REQUIRE(domain.allocations == allocations && domain.live_bytes == bytes && array == snapshot);
    domain.fail_after = budget;
    write = xr_mvl_array_access_write(access, 0, 8, true);
    if (budget == SIZE_MAX) {
        MVL_REQUIRE(write.status == XR_MVL_OK);
        pending = true;
        *sites = domain.allocations - allocations;
        MVL_REQUIRE(*sites == 2 && write.previous == array && array == snapshot);
        MVL_REQUIRE(xr_signed_rc_read(&array->header.refs) == (negative ? -2 : 2));
        MVL_REQUIRE(write.access.owner != array && write.access.owner->data[1] == 8);
        MVL_REQUIRE(xr_signed_rc_read(&write.access.owner->header.refs) == 1);
        if (abort_write) {
            xr_mvl_drop(&write.access.owner->header);
            pending = false;
            MVL_REQUIRE(array == snapshot && array->length == 1 && array->data[0] == 7);
            MVL_REQUIRE(domain.live_bytes == bytes && domain.live_blocks == blocks);
        } else {
            mvl_access_commit(&array, &access, write);
            pending = false;
            MVL_REQUIRE(snapshot->length == 1 && snapshot->data[0] == 7);
            MVL_REQUIRE(xr_signed_rc_read(&snapshot->header.refs) == (negative ? -1 : 1));
            write = xr_mvl_array_access_push(access, 9);
            MVL_REQUIRE(write.status == XR_MVL_OK && !write.previous);
            mvl_access_commit(&array, &access, write);
            MVL_REQUIRE(array->length == 3 && array->data[2] == 9 && access.length == 3);
            MVL_REQUIRE(xr_signed_rc_read(&array->header.refs) == 1);
        }
    } else {
        MVL_REQUIRE(write.status == XR_MVL_OOM && !write.previous && !write.access.writable);
        MVL_REQUIRE(array == snapshot && array->length == 1 && array->data[0] == 7);
        MVL_REQUIRE(domain.live_bytes == bytes && domain.live_blocks == blocks);
        MVL_REQUIRE(xr_signed_rc_read(&array->header.refs) == (negative ? -2 : 2));
    }
cleanup:
    if (pending) xr_mvl_drop(&write.access.owner->header);
    if (array) xr_mvl_drop(&array->header);
    if (snapshot) xr_mvl_drop(&snapshot->header);
    return ok && !domain.live_blocks && !domain.live_bytes;
}

static int64_t mvl_access_rhs_rebind(XrMvlArray **binding, XrMvlArray *replacement, unsigned *effects) {
    XrMvlArray *old = *binding;
    *binding = replacement;
    xr_mvl_drop(&old->header);
    ++*effects;
    return 42;
}

static int64_t mvl_access_observe(const XrMvlArray *array, unsigned *effects) {
    ++*effects;
    return array->data[0];
}

static bool mvl_verify_access_invalidation(void) {
    static XrMvlDomain domain;
    xr_mvl_domain_init(&domain);
    XrMvlArray *array = xr_mvl_array_new(&domain), *snapshot = NULL, *replacement = NULL;
    XrMvlArrayAccess access = xr_mvl_array_access_start(NULL);
    bool ok = true;
    MVL_REQUIRE(array);
    XrMvlArrayWrite write = xr_mvl_array_access_write(xr_mvl_array_access_start(array), 0, 1, true);
    MVL_REQUIRE(write.status == XR_MVL_OK);
    mvl_access_commit(&array, &access, write);
    MVL_REQUIRE(array->length == 1 && xr_signed_rc_read(&array->header.refs) == 1);
    access = xr_mvl_array_access_start(NULL);
    MVL_REQUIRE(xr_mvl_retain(&array->header));
    snapshot = array;
    write = xr_mvl_array_access_push(access, 99);
    MVL_REQUIRE(write.status == XR_MVL_INVALID_OWNER && snapshot->length == 1);
    replacement = xr_mvl_array_new(&domain);
    MVL_REQUIRE(replacement);
    unsigned effects = 0;
    int64_t value = mvl_access_rhs_rebind(&array, replacement, &effects);
    replacement = NULL;
    size_t bytes = domain.live_bytes, allocations = domain.allocations;
    domain.fail_after = 0;
    write = xr_mvl_array_access_write(xr_mvl_array_access_start(array), 0, value, false);
    MVL_REQUIRE(effects == 1 && write.status == XR_MVL_BOUNDS);
    MVL_REQUIRE(array->length == 0 && snapshot->length == 1 && snapshot->data[0] == 1);
    MVL_REQUIRE(domain.live_bytes == bytes && domain.allocations == allocations);
    write = xr_mvl_array_access_write(xr_mvl_array_access_start(array), 0, value, true);
    MVL_REQUIRE(write.status == XR_MVL_OOM && array->length == 0);
    domain.fail_after = SIZE_MAX;
    write = xr_mvl_array_access_write(xr_mvl_array_access_start(array), 0, value, true);
    MVL_REQUIRE(write.status == XR_MVL_OK);
    mvl_access_commit(&array, &access, write);
    MVL_REQUIRE(array->length == 1 && array->data[0] == 42 && snapshot->data[0] == 1);
    access = xr_mvl_array_access_start(NULL);
    /* Even an observing call closes the trusted window before it executes. */
    int64_t observed = mvl_access_observe(array, &effects);
    write = xr_mvl_array_access_push(access, observed);
    MVL_REQUIRE(write.status == XR_MVL_INVALID_OWNER && effects == 2);
    write = xr_mvl_array_access_write(xr_mvl_array_access_start(array), 0, observed, true);
    MVL_REQUIRE(write.status == XR_MVL_OK);
    mvl_access_commit(&array, &access, write);
    MVL_REQUIRE(array->length == 2 && array->data[1] == 42);
cleanup:
    if (replacement) xr_mvl_drop(&replacement->header);
    if (array) xr_mvl_drop(&array->header);
    if (snapshot) xr_mvl_drop(&snapshot->header);
    return ok && !domain.live_blocks && !domain.live_bytes;
}

static bool mvl_verify_access_grow(void) {
    static XrMvlDomain domain;
    xr_mvl_domain_init(&domain);
    XrMvlArray *array = xr_mvl_array_new(&domain);
    bool ok = true;
    MVL_REQUIRE(array);
    for (int64_t i = 0; i < 4; ++i)
        MVL_REQUIRE(xr_mvl_array_push(&array, i) == XR_MVL_OK);
    XrMvlArrayWrite write = xr_mvl_array_access_write(xr_mvl_array_access_start(array), 0, 0, false);
    MVL_REQUIRE(write.status == XR_MVL_OK && !write.previous);
    XrMvlArrayAccess access;
    mvl_access_commit(&array, &access, write);
    int64_t *data = array->data;
    size_t bytes = domain.live_bytes, blocks = domain.live_blocks;
    domain.fail_after = 0;
    write = xr_mvl_array_access_set(access, 4, 99);
    MVL_REQUIRE(write.status == XR_MVL_BOUNDS && array->length == 4);
    write = xr_mvl_array_access_push(access, 4);
    MVL_REQUIRE(write.status == XR_MVL_OOM && !write.previous);
    MVL_REQUIRE(array->data == data && array->length == 4 && array->capacity == 4);
    MVL_REQUIRE(domain.live_bytes == bytes && domain.live_blocks == blocks);
    access = xr_mvl_array_access_start(NULL);
    domain.fail_after = SIZE_MAX;
    write = xr_mvl_array_access_write(xr_mvl_array_access_start(array), 0, 4, true);
    MVL_REQUIRE(write.status == XR_MVL_OK);
    mvl_access_commit(&array, &access, write);
    MVL_REQUIRE(array->length == 5 && array->data[4] == 4 && access.data == array->data);
    MVL_REQUIRE(access.capacity == array->capacity && array->capacity == 8);
    write = xr_mvl_array_access_set(access, 4, 40);
    MVL_REQUIRE(write.status == XR_MVL_OK && array->data[4] == 40 && array->length == 5);
cleanup:
    if (array) xr_mvl_drop(&array->header);
    return ok && !domain.live_blocks && !domain.live_bytes;
}

static bool mvl_verify_access(void) {
    for (unsigned negative = 0; negative < 2; ++negative) {
        size_t sites = 0;
        if (!mvl_verify_access_cow(SIZE_MAX, negative != 0, false, &sites) ||
            !mvl_verify_access_cow(SIZE_MAX, negative != 0, true, &sites))
            return false;
        for (size_t budget = 0; budget < sites; ++budget)
            if (!mvl_verify_access_cow(budget, negative != 0, false, &sites))
                return false;
    }
    return mvl_verify_access_invalidation() && mvl_verify_access_grow();
}
