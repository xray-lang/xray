/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * array_append_cost.c - Isolate two costs under one private Array lifetime
 */
#include "access.h"

static bool append_cost(XrMvlDomain *domain, int64_t *sum) {
    XrMvlArray *owner = xr_mvl_array_new(domain);
    if (!owner)
        return false;
    int64_t next = 0;
    while (next < XR_MVL_PUSH_COUNT) {
        // The only call in the loop owns real growth and sees the published length.
        XrMvlArrayWrite write = xr_mvl_array_access_write(xr_mvl_array_access_start(owner), 0, next, true);
        if (write.status != XR_MVL_OK) {
            xr_mvl_drop(&owner->header);
            return false;
        }
        owner = write.access.owner;
        if (write.previous)
            xr_mvl_drop(&write.previous->header);
        size_t length = write.access.length;
        size_t room = write.access.capacity - length;
        int64_t *data = write.access.data;
        ++next;
        size_t remaining = (size_t)(XR_MVL_PUSH_COUNT - next);
        size_t count = room < remaining ? room : remaining;
        // No reads of the Array from outside this loop are possible during a segment.
        for (size_t i = 0; i < count; ++i) {
#if ARRAY_COST_CHECK_UNIQUE
            if (!xr_signed_rc_unique(&owner->header.refs))
                abort();
#endif
            data[length + i] = next + (int64_t)i;
#if ARRAY_COST_PUBLISH_LENGTH
            owner->length = length + i + 1;
#endif
        }
        owner->length = length + count;
        next += (int64_t)count;
    }
    *sum = 0;
    for (size_t i = 0; i < owner->length; ++i)
        *sum += owner->data[i];
    xr_mvl_drop(&owner->header);
    return true;
}

int main(void) {
    XrMvlDomain domain;
    xr_mvl_domain_init(&domain);
    int64_t sum = 0;
    if (!append_cost(&domain, &sum) || domain.live_blocks || domain.live_bytes)
        return 1;
    printf("%" PRId64 "\n", sum);
    return 0;
}
