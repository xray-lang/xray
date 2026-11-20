/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_array_region_binding.h - Owned numeric projection of existing region homes
 */
#ifndef XLIR_ARRAY_REGION_BINDING_H
#define XLIR_ARRAY_REGION_BINDING_H
#include "xlir_value_homes.h"
#include "xray_native_array_local.h"

typedef struct XrLirArrayRegionMove {
    XrLirArraySetRegionStep step;
    size_t source_slot, target_slot;
} XrLirArrayRegionMove;
typedef struct XrLirArrayRegionAccess {
    uint32_t block, instruction, receiver, result, index, item;
    XrLirArrayOperation operation;
    size_t receiver_slot, workspace_slot;
} XrLirArrayRegionAccess;
typedef struct XrLirArrayRegionOperation {
    /* One-based indices; zero is absent. Slot indices below are zero-based. */
    size_t access, move, drop;
} XrLirArrayRegionOperation;
typedef struct XrLirArrayRegionBlock {
    uint32_t region;
    size_t first_operation, instruction_count, edges[2];
} XrLirArrayRegionBlock;
typedef enum XrLirArrayRegionEdgeKind {
    XR_LIR_ARRAY_REGION_EDGE_NONE, XR_LIR_ARRAY_REGION_EDGE_ENTRY,
    XR_LIR_ARRAY_REGION_EDGE_BODY, XR_LIR_ARRAY_REGION_EDGE_LATCH,
    XR_LIR_ARRAY_REGION_EDGE_EXIT
} XrLirArrayRegionEdgeKind;
typedef struct XrLirArrayRegionEdge {
    uint32_t region;
    XrLirArrayRegionEdgeKind kind;
    size_t move;
} XrLirArrayRegionEdge;
typedef struct XrLirArrayRegionOwnerHomeBinding {
    uint32_t kind;
    /* Global indices in the original slots table; NONE has all fields zero.
     * Ordinary homes and every original move remain separately available. */
    size_t stable_slot, exit_slot;
} XrLirArrayRegionOwnerHomeBinding;
typedef struct XrLirArrayRegionBinding {
    XrLirArraySetRegion region;
    size_t first_slot, slot_count, canonical_slot;
    size_t first_move, move_count, first_access, access_count, first_block, block_count;
    XrLirArrayRegionOwnerHomeBinding owner_home;
} XrLirArrayRegionBinding;
typedef struct XrLirArrayRegionBindings {
    const XrLirArrayRegionBinding *regions;
    const XrtArrayI64RegionSlot *slots;
    const XrLirArrayRegionMove *moves;
    const XrLirArrayRegionAccess *accesses;
    const uint32_t *blocks;
    const XrLirArrayRegionBlock *block_index;
    const XrLirArrayRegionOperation *operations;
    /* Indexed exactly like the source Homes edge table. */
    const XrLirArrayRegionEdge *edges;
    size_t region_count, slot_count, move_count, access_count, block_count;
    size_t block_index_count, operation_count, edge_count;
} XrLirArrayRegionBindings;

/* Form the runtime description of already bound slots. The runtime still
 * decides whether the live frame permits the borrow. */
static inline XrtArrayI64RegionSpec xr_lir_array_region_guard_spec(
    const XrLirArrayRegionBindings *bindings, const XrLirArrayRegionBinding *row) {
    return (XrtArrayI64RegionSpec){bindings->slots + row->first_slot, row->slot_count,
        row->canonical_slot - row->first_slot};
}

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

/* Project sealed same-process Homes and their exact layouts without another
 * body, CFG or ownership analysis. Output owns only numeric copies and remains
 * valid after every creator retires. No row grants runtime access. Unsupported
 * legal geometry leaves that region's slot_count zero, preserving the ordinary
 * consumer. Failure frees pending output and preserves a NULL output argument.
 * Global slot indices select slots above; canonical is global as well. Moves,
 * accesses and blocks form each region's contiguous slices. block_index uses
 * the original one-based block ID, including unused zero. */
XR_FUNC XrLirValueLayoutStatus xr_lir_array_region_bindings_create(const XrLirValueHomes *,
    const XrLirValueLayouts *, XrLirArrayRegionBindings **);
/* Project only the separate PUSH capacity facts. The same numeric containers
 * include all participating value temporaries, exact PUSH workspace and edge
 * fields. Existing executable SET/GET directories remain untouched. */
XR_FUNC XrLirValueLayoutStatus xr_lir_array_push_window_bindings_create(const XrLirValueHomes *,
    const XrLirValueLayouts *, XrLirArrayRegionBindings **);
XR_FUNC void xr_lir_array_region_bindings_destroy(XrLirArrayRegionBindings *);
#endif
