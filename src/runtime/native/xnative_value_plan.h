/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_value_plan.h - Independent typed metadata for inline owned values
 *
 * KEY CONCEPT:
 *   Immutable plans own receiving descriptions and code leases. Inline payloads
 *   own only their declared Header leaves, without allocating a record box.
 */
#ifndef XNATIVE_VALUE_PLAN_H
#define XNATIVE_VALUE_PLAN_H
#include "xnative_managed.h"
#include "shared/xr_param_mode.h"
#include "xray_native_value_image.h"

typedef struct XrtValuePlan XrtValuePlan;
typedef struct XrtValueSlotSpec {
    const XrtValuePlan *type;
    size_t offset;
    const char *name;
    size_t name_length;
} XrtValueSlotSpec;
typedef struct XrtValueParameterSpec {
    const XrtValuePlan *type;
    XrParamMode mode;
    bool escaping;
    const char *name;
    size_t name_length;
} XrtValueParameterSpec;
typedef struct XrtValueSignatureSpec {
    const XrtValueParameterSpec *parameters;
    size_t parameter_count;
    const XrtValuePlan *result;
    uint32_t flags;
    bool throws;
} XrtValueSignatureSpec;
typedef struct XrtValueNullableSpec {
    size_t tag_offset, payload_offset;
    const XrtValuePlan *element;
} XrtValueNullableSpec;
typedef struct XrtValueEntrySpec {
    uint32_t ordinal;
    const XrtValuePlan *signature;
} XrtValueEntrySpec;
typedef struct XrtValuePlanSpec {
    XrtValueKind kind;
    size_t size, alignment, count;
    const size_t *owned_offsets;
    size_t owned_count;
    const XrtValueSlotSpec *fields;
    size_t field_count;
    XrtValueNullableSpec nullable;
    const XrtValueSlotSpec *captures;
    size_t capture_count;
    XrtValueSignatureSpec signature;
    XrtValueEntrySpec entry;
    const XrtValuePlan *element; /* Cell or class payload, or the exact inline Array row. */
    const XrtValuePlan *class_parent;
    XrtManagedLease code;
} XrtValuePlanSpec;

/* Earlier plans are independent immutable runtime owners, never compiler pool
 * rows. The constructor copies all vectors and name bytes, retains every plan
 * dependency and takes its own optional code lease. Code context must be an
 * independent owner of executable code, unbound raw constants/bytes and
 * complete read-only descriptions; bound managed payloads are forbidden. It
 * must not own Layout, State, ValuePlan, Ops or value RuntimeTypePlan objects.
 * An entry ordinal describes metadata and grants no executable address.
 * Plans describe the admitted subset; they grant no new language admission,
 * escaping permission, exclusive mutation authority or reference-cycle support.
 * An empty record has one private C11 padding byte, alignment one and no owned
 * leaves; its logical field count stays zero. Metadata references remain in
 * the shared counter band. */
XR_FUNC XrtManagedStatus xrt_value_plan_new(XrtBillingOwner *, const XrtValuePlanSpec *, XrtValuePlan **empty);
XR_FUNC bool xrt_value_plan_retain(XrtValuePlan *);
XR_FUNC void xrt_value_plan_drop(XrtValuePlan *);
/* All returned slices and dependency pointers borrow the retained plan. */
XR_FUNC XrtManagedStatus xrt_value_plan_view(const XrtValuePlan *, XrtValuePlanSpec *);
/* This describes, but does not acquire, a reference. RuntimeTypePlan takes its
 * own reference when it receives the lease; ValuePlan never owns that plan. */
XR_FUNC XrtManagedLease xrt_value_plan_lease(XrtValuePlan *);

/* Fixed-size object backing for an inline payload following its Header.
 * Shifted offsets borrow scratch through binding, then belong to Ops. */
XR_FUNC XrtManagedStatus xrt_inline_payload_backing_bind(XrtBillingOwner *,
    const XrtValuePlanSpec *, size_t *shifted_offsets, XrtManagedLease, XrtManagedOps **empty);
XR_FUNC XrtManagedStatus xrt_class_backing_bind(XrtBillingOwner *,
    const XrtValuePlan *class_plan, XrtManagedOps **empty);
/* The row is complete and borrows its input owners. The destination is a real
 * empty Frame slot and remains empty on failure. */
XR_FUNC XrtManagedStatus xrt_class_create(XrtManagedFrame *, size_t slot,
    XrtManagedOps *, const void *row);
XR_FUNC bool xrt_class_backing_matches(const XrtManagedHeader *, const XrtValuePlan *expected);
/* The live owner and exact nominal plan must remain live through a field
 * borrow. A caller taking an independent value copies it before dropping the
 * class, using the returned field plan. */
XR_FUNC XrtManagedStatus xrt_class_field_borrow(const XrtManagedHeader *,
    const XrtValuePlan *expected, size_t index, const XrtValuePlan **empty_type,
    const void **empty_payload);

/* Payload memory has the plan's size/alignment and a real active lifetime.
 * Zero is only for storage without live owners; it initializes every owned
 * pointer to true NULL. Retire live owners with drop rather than zero. Empty destinations have
 * NULL in all owned slots; other bytes may be scratch. Copy/move reject exact
 * and partial overlap and allocate nothing. Argument rejection preserves both
 * inputs; a failed retain preserves source and leaves destination canonically
 * zero with true NULL owners.
 * Copy retains every source owner before writing any destination byte. Move
 * transfers owners and zeroes the complete source. The caller keeps source,
 * destination and plan exclusively available through the operation; READ
 * borrows and observers cannot overlap this mutation. */
XR_FUNC XrtManagedStatus xrt_value_zero(const XrtValuePlan *, void *);
XR_FUNC XrtManagedStatus xrt_value_copy_empty(const XrtValuePlan *, const void *, void *);
XR_FUNC XrtManagedStatus xrt_value_move_empty(const XrtValuePlan *, void *, void *);
/* Each owned slot is cleared before dropping its real reference. A cleanup
 * callback can observe NULL retired siblings and still-live unvisited siblings,
 * never a freed sibling still installed in a slot. It must not mutate this
 * payload or retire its borrowed plan. Payload bytes finish empty. No payload
 * callback interprets tags or recurses into records. */
XR_FUNC void xrt_value_drop(const XrtValuePlan *, void *);
#endif // XNATIVE_VALUE_PLAN_H
