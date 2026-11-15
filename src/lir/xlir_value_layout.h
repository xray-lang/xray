/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_value_layout.h - One owned fixed-C projection of admitted LIR value types
 *
 * KEY CONCEPT:
 *   C serialization and bytecode preparation consume the same receiving plans.
 *   Type-pool ordinals identify this snapshot, never another module's types.
 */
#ifndef XLIR_VALUE_LAYOUT_H
#define XLIR_VALUE_LAYOUT_H
#include "xlir_types.h"
#include "xlir_ownership.h"
#include "runtime/native/xnative_value_plan.h"

typedef struct XrLirValueLayouts XrLirValueLayouts;
typedef struct XrLirValueLayoutKey {
    XrLirType type;
    XrLirType array_element;
} XrLirValueLayoutKey;
typedef enum XrLirValueLayoutStatus {
    XR_LIR_VALUE_LAYOUT_OK, XR_LIR_VALUE_LAYOUT_ARGUMENT, XR_LIR_VALUE_LAYOUT_UNSUPPORTED,
    XR_LIR_VALUE_LAYOUT_OOM, XR_LIR_VALUE_LAYOUT_LIMIT, XR_LIR_VALUE_LAYOUT_CLOSED,
    XR_LIR_VALUE_LAYOUT_REFCOUNT, XR_LIR_VALUE_LAYOUT_OVERFLOW
} XrLirValueLayoutStatus;

/* Consume a finished, already admitted LIR module without redoing language
 * admission. Array's exact element is an explicit existing shape fact; every
 * other key requires array_element=0. Only requested roots and their earlier
 * type dependencies are projected. Records and Nullable use natural inline C11
 * layout, not the old boxed transport. Empty Records have one private storage
 * byte and no language fields. Fn/Cell/capability projections are explicitly
 * unsupported here, not newly rejected by LIR.
 * Common C storage declares every owned leaf as XrtManagedHeader *; typed
 * String/Array/Map pointers are borrowed casts, never aliased owner fields.
 * Output starts NULL and is unchanged on failure. All persistent and temporary
 * metadata is billed through the supplied owner and refunded on failure.
 * Names, dependencies and offsets are independently owned; no module pointer
 * survives creation. A retained child plan survives this set and its creator. */
XR_FUNC XrLirValueLayoutStatus xr_lir_value_layouts_create(const XrLirModule *,
    const XrLirValueLayoutKey *roots, size_t count, XrtBillingOwner *, XrLirValueLayouts **empty);
XR_FUNC void xr_lir_value_layouts_destroy(XrLirValueLayouts *);
XR_FUNC size_t xr_lir_value_layouts_count(const XrLirValueLayouts *);
/* These return borrowed plans. Retain one before retiring the set. Enumeration
 * follows dependency-before-parent generation order and preserves outputs on
 * rejection; a numeric key does not authorize reuse with a rebuilt module. */
XR_FUNC XrLirValueLayoutStatus xr_lir_value_layouts_at(const XrLirValueLayouts *, size_t,
    XrLirValueLayoutKey *, const XrtValuePlan **);
XR_FUNC XrLirValueLayoutStatus xr_lir_value_layouts_get(const XrLirValueLayouts *,
    XrLirValueLayoutKey, const XrtValuePlan **);
/* Only generated Record/Nullable rows occur here, including false for inline
 * POD. These facts are copied from the receiving plans' actual owned_count,
 * not inferred again by the lifetime verifier or either execution backend. */
XR_FUNC XrLirValueLayoutStatus xr_lir_value_layouts_ownership(const XrLirValueLayouts *,
    const XrLirOwnershipTypeFact **types, uint32_t *count);
#endif // XLIR_VALUE_LAYOUT_H
