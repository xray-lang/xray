/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_fixed_array_guard.h - Runtime guard over the common Array homes
 */
#ifndef XLIR_FIXED_ARRAY_GUARD_H
#define XLIR_FIXED_ARRAY_GUARD_H
#include "xlir_value_homes.h"
#include "xray_native_array_local.h"

typedef enum XrLirArrayRegionEdgeKind {
    XR_LIR_ARRAY_REGION_EDGE_NONE, XR_LIR_ARRAY_REGION_EDGE_ENTRY,
    XR_LIR_ARRAY_REGION_EDGE_BODY, XR_LIR_ARRAY_REGION_EDGE_LATCH,
    XR_LIR_ARRAY_REGION_EDGE_EXIT
} XrLirArrayRegionEdgeKind;
enum { XR_LIR_FIXED_ARRAY_GUARD_SLOT_COUNT = 2 };

static inline XrtArrayI64RegionSpec xr_lir_fixed_array_guard_spec(const XrLirValueHomes *homes,
    const XrLirFixedArrayUpdateHome *row, XrtArrayI64RegionSlot slots[XR_LIR_FIXED_ARRAY_GUARD_SLOT_COUNT]) {
    const size_t *offsets = homes->frame.owned_offsets;
    slots[0] = (XrtArrayI64RegionSlot){row->owner_slot, offsets[row->owner_slot]};
    if (row->workspace_slot == SIZE_MAX) {
        slots[1] = (XrtArrayI64RegionSlot){0};
        return (XrtArrayI64RegionSpec){slots, 1, 0};
    }
    slots[1] = (XrtArrayI64RegionSlot){row->workspace_slot, offsets[row->workspace_slot]};
    return (XrtArrayI64RegionSpec){slots, XR_LIR_FIXED_ARRAY_GUARD_SLOT_COUNT, 0};
}

#endif
