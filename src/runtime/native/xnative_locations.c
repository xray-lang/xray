/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_locations.c - Owned location tables independent of compiler storage
 *
 * KEY CONCEPT:
 *   One allocation contains the immutable table and all explicit path bytes.
 *   A live reference protects both; no source-map global or compiler is needed.
 */
#include "xnative_locations.h"
#include "base/xchecks.h"
#include "base/xmalloc.h"
#include <stdatomic.h>

struct XrtLocationOwner {
    atomic_size_t references;
    size_t count;
    const XrtSourceLocation *locations;
};
_Static_assert(_Alignof(XrtLocationOwner) >= _Alignof(XrtSourceLocation),
    "The location table following its owner must remain aligned");

static bool location_valid(const XrtSourceLocation *location) {
    if (!location->path || !location->path_length || !location->line || !location->column)
        return false;
    if (!location->end_line && !location->end_column)
        return true;
    return location->end_line && location->end_column &&
        location->end_line >= location->line &&
        (location->end_line != location->line || location->end_column >= location->column);
}

XR_FUNC XrtLocationStatus xrt_locations_create(const XrtSourceLocation *locations,
    size_t count, XrtLocationOwner **output) {
    if (!output || *output || (count == 0) != (locations == NULL))
        return XRT_LOCATION_INVALID;
    if (count > UINT32_MAX || count > (SIZE_MAX - sizeof(XrtLocationOwner)) / sizeof(*locations))
        return XRT_LOCATION_LIMIT;
    size_t bytes = sizeof(XrtLocationOwner) + count * sizeof(*locations);
    for (size_t i = 0; i < count; ++i) {
        if (!location_valid(&locations[i]))
            return XRT_LOCATION_INVALID;
        if (locations[i].path_length > SIZE_MAX - bytes)
            return XRT_LOCATION_LIMIT;
        bytes += locations[i].path_length;
    }
    XrtLocationOwner *owner = xr_malloc(bytes);
    if (!owner)
        return XRT_LOCATION_OUT_OF_MEMORY;
    atomic_init(&owner->references, 1);
    owner->count = count;
    XrtSourceLocation *table = (XrtSourceLocation *)(owner + 1);
    owner->locations = table;
    char *path = (char *)(table + count);
    for (size_t i = 0; i < count; ++i) {
        table[i] = locations[i];
        memcpy(path, locations[i].path, locations[i].path_length);
        table[i].path = path;
        path += locations[i].path_length;
    }
    *output = owner;
    return XRT_LOCATION_OK;
}

XR_FUNC bool xrt_locations_retain(XrtLocationOwner *owner) {
    if (!owner)
        return false;
    size_t references = atomic_load_explicit(&owner->references, memory_order_relaxed);
    while (references && references != SIZE_MAX) {
        if (atomic_compare_exchange_weak_explicit(&owner->references, &references,
                references + 1, memory_order_relaxed, memory_order_relaxed))
            return true;
    }
    return false;
}

XR_FUNC void xrt_locations_release(XrtLocationOwner *owner) {
    if (!owner)
        return;
    size_t previous = atomic_fetch_sub_explicit(&owner->references, 1, memory_order_acq_rel);
    XR_CHECK(previous, "A location reference must remain live until release");
    if (previous == 1)
        xr_free(owner);
}

XR_FUNC size_t xrt_locations_count(const XrtLocationOwner *owner) {
    return owner ? owner->count : 0;
}

XR_FUNC const XrtSourceLocation *xrt_locations_get(const XrtLocationOwner *owner, uint32_t id) {
    return owner && id && (size_t)id <= owner->count ? &owner->locations[id - 1] : NULL;
}
