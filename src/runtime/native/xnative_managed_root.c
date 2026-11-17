/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_managed_root.c - Metered activation owners with failure-atomic publication
 *
 * KEY CONCEPT:
 *   Heap frames survive longjmp; pending allocations retire before committed parents.
 */
#include "xnative_managed_root.h"
#include "base/xchecks.h"
#include "base/xmax_align.h"
#include <stdalign.h>

_Static_assert(offsetof(XrtManagedSlot,pending) == sizeof(XrtManagedHeader *) &&
    offsetof(XrtManagedSlot,owner_offset) == sizeof(XrtManagedHeader *) + sizeof(XrtManagedPending) &&
    sizeof(XrtManagedSlot) == sizeof(XrtManagedHeader *) + sizeof(XrtManagedPending) + sizeof(size_t),
    "canonical slot metadata preserves its original three-field layout");
struct XrtManagedFrame {
    XrtManagedHeader header;
    XrtManagedRoot *root;
    XrtManagedFrame *previous;
    size_t slot_count, payload_offset, payload_size, tail_bytes;
};
_Static_assert(sizeof(XrtManagedFrame) % alignof(XrtManagedSlot) == 0 &&
    alignof(XrtManagedFrame) >= alignof(XrtManagedSlot), "frame tail has slot alignment");
struct XrtManagedRoot {
    XrtManagedHeader header;
    XrtManagedFrame *top;
    XrtManagedOps *frame_ops;
    bool busy;
    const XrtManagedSlotReservation *reservation;
    XrtManagedFrame *reserved_frame;
    size_t reserved_index;
    XrtManagedSlotLocation reserved_binding;
    unsigned reserved_phase;
};

/* The allocation owns a fixed, checked tail, not an independently movable bank. */
static XrtManagedSlot *frame_slots(XrtManagedFrame *frame) {
    return (XrtManagedSlot *)(frame + 1);
}
static XrtManagedHeader **frame_owner(XrtManagedFrame *frame, XrtManagedSlot *slot) {
    return slot->owner_offset == SIZE_MAX ? &slot->committed :
        (XrtManagedHeader **)((unsigned char *)frame + frame->payload_offset + slot->owner_offset);
}
static const uint32_t cell_guard = UINT32_C(1);
XR_FUNCDEF bool xrt_managed_domain_valid(XrtManagedSlotLocation domain) {
    return !!domain.busy != !!domain.cell_domain;
}
XR_FUNCDEF bool xrt_managed_domain_same(XrtManagedSlotLocation left, XrtManagedSlotLocation right) {
    return xrt_managed_domain_valid(left) && xrt_managed_domain_valid(right) &&
        left.busy == right.busy && left.cell_domain == right.cell_domain;
}
XR_FUNCDEF bool xrt_managed_domain_held(XrtManagedSlotLocation domain) {
    if (!xrt_managed_domain_valid(domain)) return false;
    return domain.busy ? *domain.busy :
        (atomic_load_explicit(&domain.cell_domain->flags, memory_order_acquire) & cell_guard) != 0;
}
XR_FUNCDEF XrtManagedStatus xrt_managed_domain_enter(XrtManagedSlotLocation domain) {
    if (!xrt_managed_domain_valid(domain)) return XRT_MANAGED_ARGUMENT;
    if (domain.busy) {
        if (*domain.busy) return XRT_MANAGED_ARGUMENT;
        *domain.busy = true; return XRT_MANAGED_OK;
    }
    XrtManagedHeader *cell = domain.cell_domain;
    if (!xrt_managed_retain(cell)) return XRT_MANAGED_REFCOUNT;
    uint32_t flags = atomic_load_explicit(&cell->flags, memory_order_relaxed);
    while (!(flags & cell_guard)) {
        if (atomic_compare_exchange_weak_explicit(&cell->flags, &flags, flags | cell_guard,
            memory_order_acquire, memory_order_relaxed)) return XRT_MANAGED_OK;
    }
    xrt_managed_drop(cell);
    return XRT_MANAGED_ARGUMENT;
}
XR_FUNCDEF void xrt_managed_domain_leave(XrtManagedSlotLocation domain) {
    XR_CHECK(xrt_managed_domain_held(domain), "only an acquired actual domain can leave");
    if (domain.busy) *domain.busy = false;
    else {
        atomic_fetch_and_explicit(&domain.cell_domain->flags, ~cell_guard, memory_order_release);
        xrt_managed_drop(domain.cell_domain);
    }
}
static void frame_abort(XrtManagedFrame *frame) {
    for (size_t i = 0; i < frame->slot_count; ++i)
        xrt_managed_abort(&frame_slots(frame)[i].pending);
}
static void frame_dispose(XrtManagedHeader *value, void *context) {
    (void)context;
    XrtManagedFrame *frame = (XrtManagedFrame *)value;
    frame_abort(frame);
    for (size_t i = 0; i < frame->slot_count; ++i) {
        XrtManagedHeader **owner = frame_owner(frame, &frame_slots(frame)[i]);
        XrtManagedHeader *owned = *owner;
        *owner = NULL;
        xrt_managed_drop(owned);
    }
}
static void root_clear(XrtManagedRoot *root) {
    XR_CHECK(root->busy, "exclusive mutation protects the active frame chain");
    /* Pending payloads may depend on still-committed parents in another frame. */
    for (XrtManagedFrame *frame = root->top; frame; frame = frame->previous)
        frame_abort(frame);
    while (root->top) {
        XrtManagedFrame *frame = root->top;
        root->top = frame->previous;
        frame->root = NULL;
        frame->previous = NULL;
        xrt_managed_drop(&frame->header);
    }
}
static void root_dispose(XrtManagedHeader *value, void *context) {
    (void)context;
    XrtManagedRoot *root = (XrtManagedRoot *)value;
    root->busy = true;
    root_clear(root);
    xrt_managed_ops_drop(root->frame_ops);
    root->frame_ops = NULL;
}
static bool root_begin(XrtManagedRoot *root) {
    if (!root || root->busy) return false;
    root->busy = true;
    return true;
}
static XrtManagedSlot *slot_begin(XrtManagedFrame *frame, size_t index) {
    if (!frame || !frame->root || index >= frame->slot_count || !root_begin(frame->root))
        return NULL;
    return &frame_slots(frame)[index];
}
static bool slot_empty(XrtManagedFrame *frame, XrtManagedSlot *slot) {
    return !*frame_owner(frame, slot) && !slot->pending.value && !slot->pending.allocation_bytes;
}

XRAY_API XrtManagedStatus xrt_managed_root_new(XrtBillingOwner *billing,
    XrtManagedRoot **output) {
    if (!billing || !output || *output) return XRT_MANAGED_ARGUMENT;
    const XrtRuntimeTypeSpec root_spec = {.fixed_size=sizeof(XrtManagedRoot),
        .alignment=alignof(XrtManagedRoot), .capacity_offset=SIZE_MAX, .drop_payload=root_dispose};
    const XrtRuntimeTypeSpec frame_spec = {.fixed_size=sizeof(XrtManagedFrame),
        .alignment=alignof(XrMaxAlignment), .capacity_offset=offsetof(XrtManagedFrame, tail_bytes),
        .stride=1, .drop_payload=frame_dispose};
    XrtRuntimeTypePlan *root_plan = NULL, *frame_plan = NULL;
    XrtManagedOps *root_ops = NULL, *frame_ops = NULL;
    XrtManagedPending pending = {0};
    XrtManagedStatus status = xrt_runtime_type_plan_new(billing, &root_spec, &root_plan);
    if (status == XRT_MANAGED_OK) status = xrt_runtime_type_plan_new(billing, &frame_spec, &frame_plan);
    if (status == XRT_MANAGED_OK) status = xrt_managed_ops_bind(billing, root_plan, &root_ops);
    if (status == XRT_MANAGED_OK) status = xrt_managed_ops_bind(billing, frame_plan, &frame_ops);
    if (status == XRT_MANAGED_OK) status = xrt_managed_prepare(root_ops, sizeof(XrtManagedRoot), &pending);
    if (status == XRT_MANAGED_OK) {
        XrtManagedRoot *root = (XrtManagedRoot *)pending.value;
        root->top = NULL;
        root->busy = false;
        root->reservation = NULL;
        root->reserved_frame = NULL;
        root->reserved_index = 0;
        root->reserved_binding = (XrtManagedSlotLocation){0};
        root->reserved_phase = 0;
        root->frame_ops = frame_ops;
        frame_ops = NULL;
        XrtManagedHeader *committed = NULL;
        status = xrt_managed_commit(&pending, &committed);
        if (status == XRT_MANAGED_OK) *output = (XrtManagedRoot *)committed;
    }
    xrt_managed_abort(&pending);
    xrt_managed_ops_drop(frame_ops);
    xrt_managed_ops_drop(root_ops);
    xrt_runtime_type_plan_drop(frame_plan);
    xrt_runtime_type_plan_drop(root_plan);
    return status;
}
XRAY_API XrtManagedStatus xrt_managed_root_drain(XrtManagedRoot *root) {
    if (!root_begin(root)) return XRT_MANAGED_ARGUMENT;
    root_clear(root);
    root->busy = false;
    return XRT_MANAGED_OK;
}
XRAY_API XrtManagedStatus xrt_managed_root_destroy(XrtManagedRoot **owned) {
    if (!owned) return XRT_MANAGED_ARGUMENT;
    if (!*owned) return XRT_MANAGED_OK;
    XrtManagedRoot *root = *owned;
    if (!root_begin(root)) return XRT_MANAGED_ARGUMENT;
    *owned = NULL;
    xrt_managed_drop(&root->header);
    return XRT_MANAGED_OK;
}
static XrtManagedStatus frame_extent(const XrtManagedFrameSpec *spec,
    size_t *payload_offset, size_t *bytes) {
    if (!spec || (!spec->slot_count && !spec->payload_size) ||
        (!spec->slot_count && spec->owned_offsets) || !spec->payload_alignment ||
        spec->payload_alignment > alignof(XrMaxAlignment) ||
        (spec->payload_alignment & (spec->payload_alignment - 1))) return XRT_MANAGED_ARGUMENT;
    if (spec->slot_count > (SIZE_MAX - sizeof(XrtManagedFrame)) / sizeof(XrtManagedSlot))
        return XRT_MANAGED_OVERFLOW;
    size_t start = sizeof(XrtManagedFrame) + spec->slot_count * sizeof(XrtManagedSlot);
    size_t padding = (spec->payload_alignment - start % spec->payload_alignment) % spec->payload_alignment;
    if (start > SIZE_MAX - padding || spec->payload_size > SIZE_MAX - start - padding)
        return XRT_MANAGED_OVERFLOW;
    size_t previous_end = 0;
    for (size_t i = 0; i < spec->slot_count; ++i) {
        size_t offset = spec->owned_offsets ? spec->owned_offsets[i] : SIZE_MAX;
        if (offset == SIZE_MAX) continue;
        if (spec->payload_alignment < alignof(XrtManagedHeader *) ||
            offset % alignof(XrtManagedHeader *) || offset < previous_end ||
            offset > spec->payload_size || sizeof(XrtManagedHeader *) > spec->payload_size - offset)
            return XRT_MANAGED_ARGUMENT;
        previous_end = offset + sizeof(XrtManagedHeader *);
    }
    *payload_offset = start + padding;
    *bytes = *payload_offset + spec->payload_size;
    return XRT_MANAGED_OK;
}
XRAY_API XrtManagedStatus xrt_managed_frame_enter_layout(XrtManagedRoot *root,
    const XrtManagedFrameSpec *spec, XrtManagedFrame **output) {
    if (!output || *output) return XRT_MANAGED_ARGUMENT;
    size_t payload_offset = 0, bytes = 0;
    XrtManagedStatus status = frame_extent(spec, &payload_offset, &bytes);
    if (status != XRT_MANAGED_OK) return status;
    if (!root_begin(root)) return XRT_MANAGED_ARGUMENT;
    XrtManagedPending pending = {0};
    status = xrt_managed_prepare(root->frame_ops, bytes, &pending);
    if (status == XRT_MANAGED_OK) {
        XrtManagedFrame *frame = (XrtManagedFrame *)pending.value;
        frame->root = root;
        frame->previous = root->top;
        frame->slot_count = spec->slot_count;
        frame->payload_offset = payload_offset;
        frame->payload_size = spec->payload_size;
        for (size_t i = 0; i < spec->slot_count; ++i) {
            XrtManagedSlot *slot = &frame_slots(frame)[i];
            *slot = (XrtManagedSlot){.owner_offset=spec->owned_offsets ? spec->owned_offsets[i] : SIZE_MAX};
            *frame_owner(frame, slot) = NULL;
        }
        XrtManagedHeader *committed = NULL;
        status = xrt_managed_commit(&pending, &committed);
        if (status == XRT_MANAGED_OK) root->top = *output = (XrtManagedFrame *)committed;
    }
    xrt_managed_abort(&pending);
    root->busy = false;
    return status;
}
XRAY_API XrtManagedStatus xrt_managed_frame_enter(XrtManagedRoot *root, size_t count,
    XrtManagedFrame **output) {
    /* Keep the slots-only rejection boundary while sharing the physical owner. */
    if (!count || count > (SIZE_MAX - sizeof(XrtManagedFrame)) / sizeof(XrtManagedSlot))
        return XRT_MANAGED_ARGUMENT;
    XrtManagedFrameSpec spec = {count, 0, 1, NULL};
    return xrt_managed_frame_enter_layout(root, &spec, output);
}
XRAY_API XrtManagedStatus xrt_managed_frame_payload(XrtManagedFrame *frame,
    void **output, size_t *bytes) {
    if (!frame || !frame->root || !output || *output || !bytes) return XRT_MANAGED_ARGUMENT;
    if (frame->payload_size) *output = (unsigned char *)frame + frame->payload_offset;
    *bytes = frame->payload_size;
    return XRT_MANAGED_OK;
}
XRAY_API XrtManagedStatus xrt_managed_frame_enter_payload(XrtManagedRoot *root,
    const XrtManagedFrameSpec *spec, XrtManagedFrame **output, void **payload) {
    if (!output || *output || !payload || *payload || (void *)output == (void *)payload)
        return XRT_MANAGED_ARGUMENT;
    XrtManagedFrame *frame = NULL;
    void *bytes = NULL;
    size_t extent = 0;
    XrtManagedStatus status = xrt_managed_frame_enter_layout(root, spec, &frame);
    if (status != XRT_MANAGED_OK) return status;
    status = xrt_managed_frame_payload(frame, &bytes, &extent);
    if (status != XRT_MANAGED_OK) {
        XrtManagedStatus cleanup = xrt_managed_frame_leave(root, frame);
        return cleanup == XRT_MANAGED_OK ? status : cleanup;
    }
    *output = frame;
    *payload = bytes;
    return XRT_MANAGED_OK;
}
XRAY_API XrtManagedStatus xrt_managed_frame_enter_locations(XrtManagedRoot *root,
    const XrtManagedFrameSpec *spec, XrtManagedFrameLocations *output) {
    if (!output || output->frame || output->payload || output->slots || output->busy || output->slot_count)
        return XRT_MANAGED_ARGUMENT;
    XrtManagedFrame *frame = NULL;
    XrtManagedStatus status = xrt_managed_frame_enter_layout(root,spec,&frame);
    if (status != XRT_MANAGED_OK) return status;
    *output = (XrtManagedFrameLocations){.frame=frame,
        .payload=frame->payload_size ? (unsigned char *)frame + frame->payload_offset : NULL,
        .slots=frame->slot_count ? frame_slots(frame) : NULL,
        .busy=&root->busy,.slot_count=frame->slot_count};
    return XRT_MANAGED_OK;
}
XRAY_API XrtManagedStatus xrt_managed_frame_leave(XrtManagedRoot *root, XrtManagedFrame *frame) {
    if (!frame || frame->root != root || !root || root->top != frame || !root_begin(root))
        return XRT_MANAGED_ARGUMENT;
    root->top = frame->previous;
    frame->root = NULL;
    frame->previous = NULL;
    xrt_managed_drop(&frame->header);
    root->busy = false;
    return XRT_MANAGED_OK;
}
XRAY_API XrtManagedStatus xrt_managed_slot_adopt(XrtManagedFrame *frame, size_t index,
    XrtManagedHeader **owned) {
    if (!owned || !*owned) return XRT_MANAGED_ARGUMENT;
    XrtManagedSlot *slot = slot_begin(frame, index);
    if (!slot) return XRT_MANAGED_ARGUMENT;
    XrtManagedStatus status = slot_empty(frame, slot) ? XRT_MANAGED_OK : XRT_MANAGED_ARGUMENT;
    if (status == XRT_MANAGED_OK) { *frame_owner(frame, slot) = *owned; *owned = NULL; }
    frame->root->busy = false;
    return status;
}
XRAY_API XrtManagedStatus xrt_managed_slot_borrow(XrtManagedFrame *frame, size_t index,
    XrtManagedHeader **output) {
    if (!output || *output) return XRT_MANAGED_ARGUMENT;
    if (!frame || !frame->root || index >= frame->slot_count) return XRT_MANAGED_ARGUMENT;
    *output = *frame_owner(frame, &frame_slots(frame)[index]);
    return XRT_MANAGED_OK;
}
XRAY_API XrtManagedStatus xrt_managed_slot_replace(XrtManagedFrame *frame, size_t index,
    XrtManagedHeader **owned) {
    if (!owned || !*owned) return XRT_MANAGED_ARGUMENT;
    XrtManagedSlot *slot = slot_begin(frame, index);
    if (!slot) return XRT_MANAGED_ARGUMENT;
    XrtManagedStatus status = slot->pending.value || slot->pending.allocation_bytes ?
        XRT_MANAGED_ARGUMENT : XRT_MANAGED_OK;
    if (status == XRT_MANAGED_OK) {
        XrtManagedHeader *old = *frame_owner(frame, slot);
        *frame_owner(frame, slot) = *owned;
        *owned = NULL;
        xrt_managed_drop(old);
    }
    frame->root->busy = false;
    return status;
}
XRAY_API XrtManagedStatus xrt_managed_slot_take(XrtManagedFrame *frame, size_t index,
    XrtManagedHeader **output) {
    if (!output || *output) return XRT_MANAGED_ARGUMENT;
    XrtManagedSlot *slot = slot_begin(frame, index);
    if (!slot) return XRT_MANAGED_ARGUMENT;
    XrtManagedStatus status = !*frame_owner(frame, slot) || slot->pending.value || slot->pending.allocation_bytes ?
        XRT_MANAGED_ARGUMENT : XRT_MANAGED_OK;
    if (status == XRT_MANAGED_OK) {
        *output = *frame_owner(frame, slot);
        *frame_owner(frame, slot) = NULL;
    }
    frame->root->busy = false;
    return status;
}
XRAY_API XrtManagedStatus xrt_managed_slot_move(XrtManagedFrame *destination, size_t to,
    XrtManagedFrame *source, size_t from) {
    if (!source || !destination || !source->root || source->root != destination->root ||
        from >= source->slot_count || to >= destination->slot_count) return XRT_MANAGED_ARGUMENT;
    XrtManagedSlot *target = slot_begin(destination, to);
    if (!target) return XRT_MANAGED_ARGUMENT;
    XrtManagedSlot *input = &frame_slots(source)[from];
    XrtManagedStatus status = !slot_empty(destination, target) || !*frame_owner(source, input) || input->pending.value ||
        input->pending.allocation_bytes ? XRT_MANAGED_ARGUMENT : XRT_MANAGED_OK;
    if (status == XRT_MANAGED_OK) { *frame_owner(destination, target) = *frame_owner(source, input); *frame_owner(source, input) = NULL; }
    destination->root->busy = false;
    return status;
}
XRAY_API XrtManagedStatus xrt_managed_slot_drop(XrtManagedFrame *frame, size_t index) {
    XrtManagedSlot *slot = slot_begin(frame, index);
    if (!slot) return XRT_MANAGED_ARGUMENT;
    XrtManagedStatus status = slot->pending.value || slot->pending.allocation_bytes ?
        XRT_MANAGED_ARGUMENT : XRT_MANAGED_OK;
    if (status == XRT_MANAGED_OK) {
        XrtManagedHeader *owned = *frame_owner(frame, slot);
        *frame_owner(frame, slot) = NULL;
        xrt_managed_drop(owned);
    }
    frame->root->busy = false;
    return status;
}
static XrtManagedStatus slot_prepare_active(XrtManagedSlot *slot, XrtManagedOps *ops,
    size_t bytes, XrtManagedHeader **output) {
    XrtManagedStatus status = xrt_managed_prepare(ops, bytes, &slot->pending);
    if (status == XRT_MANAGED_OK) *output = slot->pending.value;
    return status;
}
static XrtManagedStatus slot_commit_active(XrtManagedFrame *frame, XrtManagedSlot *slot) {
    XrtManagedHeader *replacement = NULL;
    XrtManagedStatus status = xrt_managed_commit(&slot->pending, &replacement);
    if (status == XRT_MANAGED_OK) {
        XrtManagedHeader *old = *frame_owner(frame, slot);
        *frame_owner(frame, slot) = replacement;
        xrt_managed_drop(old);
    }
    return status;
}
XRAY_API XrtManagedStatus xrt_managed_slot_prepare(XrtManagedFrame *frame, size_t index,
    XrtManagedOps *ops, size_t bytes, XrtManagedHeader **output) {
    if (!ops || !output || *output) return XRT_MANAGED_ARGUMENT;
    XrtManagedSlot *slot = slot_begin(frame, index);
    if (!slot) return XRT_MANAGED_ARGUMENT;
    XrtManagedStatus status = slot_prepare_active(slot,ops,bytes,output);
    frame->root->busy = false;
    return status;
}
XRAY_API XrtManagedStatus xrt_managed_slot_commit(XrtManagedFrame *frame, size_t index) {
    XrtManagedSlot *slot = slot_begin(frame, index);
    if (!slot) return XRT_MANAGED_ARGUMENT;
    XrtManagedStatus status = slot_commit_active(frame,slot);
    frame->root->busy = false;
    return status;
}
XRAY_API XrtManagedStatus xrt_managed_slot_abort(XrtManagedFrame *frame, size_t index) {
    XrtManagedSlot *slot = slot_begin(frame, index);
    if (!slot) return XRT_MANAGED_ARGUMENT;
    xrt_managed_abort(&slot->pending);
    frame->root->busy = false;
    return XRT_MANAGED_OK;
}
XR_FUNCDEF XrtManagedStatus xrt_managed_slot_construct(XrtManagedFrame *frame, size_t index,
    XrtManagedOps *ops, size_t bytes, const XrtManagedSlotConstruction *construction) {
    if (!ops || !construction || !construction->initialize || !construction->finish) return XRT_MANAGED_ARGUMENT;
    XrtManagedSlot *slot = slot_begin(frame, index);
    if (!slot) return XRT_MANAGED_ARGUMENT;
    XrtManagedHeader *value = NULL;
    XrtManagedStatus status = slot_prepare_active(slot, ops, bytes, &value);
    if (status == XRT_MANAGED_OK) {
        status = construction->initialize(value, construction->context);
        if (status == XRT_MANAGED_OK) status = slot_commit_active(frame, slot);
        if (status != XRT_MANAGED_OK) xrt_managed_abort(&slot->pending);
    }
    construction->finish(construction->context);
    frame->root->busy = false;
    return status;
}
XRAY_API XrtManagedStatus xrt_managed_slot_location(XrtManagedFrame *frame, size_t index,
    XrtManagedSlotLocation *output) {
    if (!output || !frame || !frame->root || index >= frame->slot_count) return XRT_MANAGED_ARGUMENT;
    XrtManagedSlot *slot = &frame_slots(frame)[index];
    *output = (XrtManagedSlotLocation){frame_owner(frame, slot), &slot->pending, &frame->root->busy, NULL};
    return XRT_MANAGED_OK;
}
XR_FUNCDEF XrtManagedRoot *xrt_managed_frame_root(XrtManagedFrame *frame) {
    return frame ? frame->root : NULL;
}

enum { RESERVED_EMPTY, RESERVED_PENDING, RESERVED_COMMITTED, RESERVED_PUBLISHED, RESERVED_CALLBACK };
static XrtManagedSlot *reserved_slot(const XrtManagedSlotReservation *handle) {
    if (!handle || !handle->root) return NULL;
    XrtManagedRoot *root=handle->root;
    XrtManagedFrame *frame=root->reserved_frame;
    if (root->reservation!=handle || !root->busy || root->reserved_phase==RESERVED_CALLBACK ||
        !frame || frame->root!=root || root->reserved_index>=frame->slot_count ||
        !root->reserved_binding.owner || !xrt_managed_domain_held(root->reserved_binding)) return NULL;
    return &frame_slots(frame)[root->reserved_index];
}
XR_FUNCDEF XrtManagedStatus xrt_managed_slot_reserved_begin(XrtManagedFrame *frame, size_t index,
    XrtManagedSlotLocation binding, XrtManagedSlotReservation *handle) {
    if (!frame || !frame->root || !handle || handle->root || index>=frame->slot_count ||
        !binding.owner || !xrt_managed_domain_held(binding) || !*binding.owner) return XRT_MANAGED_ARGUMENT;
    XrtManagedRoot *root=frame->root;
    XrtManagedSlot *slot=&frame_slots(frame)[index];
    if (!root->busy || root->reservation || !slot_empty(frame,slot) ||
        frame_owner(frame,slot)==binding.owner) return XRT_MANAGED_ARGUMENT;
    root->reservation=handle; root->reserved_frame=frame; root->reserved_index=index;
    root->reserved_binding=binding; root->reserved_phase=RESERVED_EMPTY;
    handle->root=root;
    return XRT_MANAGED_OK;
}
XR_FUNCDEF XrtManagedStatus xrt_managed_slot_reserved_apply(XrtManagedSlotReservation *handle,
    XrtManagedReservedApply apply, void *context) {
    XrtManagedSlot *slot=reserved_slot(handle);
    if (!slot || !apply || handle->root->reserved_phase!=RESERVED_EMPTY) return XRT_MANAGED_ARGUMENT;
    XrtManagedRoot *root=handle->root;
    root->reserved_phase=RESERVED_CALLBACK;
    XrtManagedStatus status=apply(*root->reserved_binding.owner,context);
    root->reserved_phase=RESERVED_EMPTY;
    return status;
}
XR_FUNCDEF XrtManagedStatus xrt_managed_slot_reserved_prepare(const XrtManagedSlotReservation *handle,
    XrtManagedOps *ops, size_t bytes, XrtManagedHeader **output) {
    XrtManagedSlot *slot=reserved_slot(handle);
    if (!slot || !ops || !output || *output || handle->root->reserved_phase!=RESERVED_EMPTY)
        return XRT_MANAGED_ARGUMENT;
    XrtManagedRoot *root=handle->root;
    root->reserved_phase=RESERVED_CALLBACK;
    XrtManagedStatus status=slot_prepare_active(slot,ops,bytes,output);
    root->reserved_phase=status==XRT_MANAGED_OK ? RESERVED_PENDING : RESERVED_EMPTY;
    return status;
}
XR_FUNCDEF XrtManagedStatus xrt_managed_slot_reserved_commit(const XrtManagedSlotReservation *handle) {
    XrtManagedSlot *slot=reserved_slot(handle);
    if (!slot || handle->root->reserved_phase!=RESERVED_PENDING) return XRT_MANAGED_ARGUMENT;
    XrtManagedRoot *root=handle->root;
    root->reserved_phase=RESERVED_CALLBACK;
    XrtManagedStatus status=slot_commit_active(root->reserved_frame,slot);
    root->reserved_phase=status==XRT_MANAGED_OK ? RESERVED_COMMITTED : RESERVED_PENDING;
    return status;
}
XR_FUNCDEF XrtManagedStatus xrt_managed_slot_reserved_abort(const XrtManagedSlotReservation *handle) {
    XrtManagedSlot *slot=reserved_slot(handle);
    if (!slot || handle->root->reserved_phase==RESERVED_COMMITTED ||
        handle->root->reserved_phase==RESERVED_PUBLISHED) return XRT_MANAGED_ARGUMENT;
    XrtManagedRoot *root=handle->root;
    root->reserved_phase=RESERVED_CALLBACK;
    xrt_managed_abort(&slot->pending);
    root->reserved_phase=RESERVED_EMPTY;
    return XRT_MANAGED_OK;
}
XR_FUNCDEF XrtManagedStatus xrt_managed_slot_reserved_publish(XrtManagedSlotReservation *handle) {
    XrtManagedSlot *slot=reserved_slot(handle);
    if (!slot || handle->root->reserved_phase!=RESERVED_COMMITTED ||
        slot->pending.value || slot->pending.allocation_bytes) return XRT_MANAGED_ARGUMENT;
    XrtManagedRoot *root=handle->root;
    XrtManagedHeader **candidate=frame_owner(root->reserved_frame,slot);
    if (!*candidate) return XRT_MANAGED_ARGUMENT;
    root->reserved_phase=RESERVED_CALLBACK;
    XrtManagedHeader *old=*root->reserved_binding.owner;
    *root->reserved_binding.owner=*candidate; *candidate=NULL;
    xrt_managed_drop(old);
    root->reserved_phase=RESERVED_PUBLISHED;
    return XRT_MANAGED_OK;
}
static XrtManagedSlot *reserved_result_slot(const XrtManagedSlotReservation *handle, size_t index) {
    if (!reserved_slot(handle)) return NULL;
    XrtManagedRoot *root = handle->root;
    XrtManagedFrame *frame = root->reserved_frame;
    if (index >= frame->slot_count || index == root->reserved_index) return NULL;
    XrtManagedSlot *result = &frame_slots(frame)[index];
    if (frame_owner(frame, result) == root->reserved_binding.owner ||
        result->pending.value || result->pending.allocation_bytes) return NULL;
    return result;
}
XR_FUNCDEF XrtManagedStatus xrt_managed_slot_reserved_result_check(
    const XrtManagedSlotReservation *handle, size_t index) {
    return reserved_result_slot(handle, index) && handle->root->reserved_phase == RESERVED_EMPTY ?
        XRT_MANAGED_OK : XRT_MANAGED_ARGUMENT;
}
XR_FUNCDEF XrtManagedStatus xrt_managed_slot_reserved_consume_publish(
    XrtManagedSlotReservation *handle, size_t index) {
    XrtManagedSlot *result = reserved_result_slot(handle, index);
    if (!result) return XRT_MANAGED_ARGUMENT;
    XrtManagedRoot *root = handle->root;
    bool replacement = root->reserved_phase == RESERVED_COMMITTED;
    if (!replacement && root->reserved_phase != RESERVED_EMPTY) return XRT_MANAGED_ARGUMENT;
    XrtManagedSlot *slot = reserved_slot(handle);
    XrtManagedHeader **candidate = frame_owner(root->reserved_frame, slot);
    if (!*root->reserved_binding.owner || slot->pending.value || slot->pending.allocation_bytes ||
        (replacement ? !*candidate : !slot_empty(root->reserved_frame, slot))) return XRT_MANAGED_ARGUMENT;
    XrtManagedHeader **destination = frame_owner(root->reserved_frame, result);
    XrtManagedHeader *old_source = *root->reserved_binding.owner, *old_result = *destination;
    XrtManagedHeader *incoming = replacement ? *candidate : old_source;
    root->reserved_phase = RESERVED_CALLBACK;
    *root->reserved_binding.owner = NULL;
    *candidate = NULL;
    *destination = incoming;
    /* Both incoming ownership and source consumption precede arbitrary final
     * release callbacks. An in-place source reference is moved, never retained. */
    xrt_managed_drop(old_result);
    if (replacement) xrt_managed_drop(old_source);
    root->reserved_phase = RESERVED_PUBLISHED;
    return XRT_MANAGED_OK;
}
XR_FUNCDEF XrtManagedStatus xrt_managed_slot_reserved_end(XrtManagedSlotReservation *handle) {
    XrtManagedSlot *slot=reserved_slot(handle);
    if (!slot) return XRT_MANAGED_ARGUMENT;
    XrtManagedRoot *root=handle->root;
    root->reserved_phase=RESERVED_CALLBACK;
    xrt_managed_abort(&slot->pending);
    XrtManagedHeader **candidate=frame_owner(root->reserved_frame,slot);
    XrtManagedHeader *discard=*candidate; *candidate=NULL;
    xrt_managed_drop(discard);
    XrtManagedSlotLocation binding=root->reserved_binding;
    root->reservation=NULL; root->reserved_frame=NULL; root->reserved_index=0;
    root->reserved_binding=(XrtManagedSlotLocation){0}; root->reserved_phase=RESERVED_EMPTY;
    handle->root=NULL;
    xrt_managed_domain_leave(binding); root->busy=false;
    return XRT_MANAGED_OK;
}
