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
#include <stdalign.h>

typedef struct XrtManagedSlot {
    XrtManagedHeader *committed;
    XrtManagedPending pending;
} XrtManagedSlot;
struct XrtManagedFrame {
    XrtManagedHeader header;
    XrtManagedRoot *root;
    XrtManagedFrame *previous;
    size_t slot_count;
};
_Static_assert(sizeof(XrtManagedFrame) % alignof(XrtManagedSlot) == 0 &&
    alignof(XrtManagedFrame) >= alignof(XrtManagedSlot), "frame tail has slot alignment");
struct XrtManagedRoot {
    XrtManagedHeader header;
    XrtManagedFrame *top;
    XrtManagedOps *frame_ops;
    bool busy;
};

/* The allocation owns a fixed, checked tail, not an independently movable bank. */
static XrtManagedSlot *frame_slots(XrtManagedFrame *frame) {
    return (XrtManagedSlot *)(frame + 1);
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
        XrtManagedHeader *owned = frame_slots(frame)[i].committed;
        frame_slots(frame)[i].committed = NULL;
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
static bool slot_empty(const XrtManagedSlot *slot) {
    return !slot->committed && !slot->pending.value && !slot->pending.allocation_bytes;
}

XR_FUNCDEF XrtManagedStatus xrt_managed_root_new(XrtBillingOwner *billing,
    XrtManagedRoot **output) {
    if (!billing || !output || *output) return XRT_MANAGED_ARGUMENT;
    const XrtRuntimeTypeSpec root_spec = {.fixed_size=sizeof(XrtManagedRoot),
        .alignment=alignof(XrtManagedRoot), .capacity_offset=SIZE_MAX, .drop_payload=root_dispose};
    const XrtRuntimeTypeSpec frame_spec = {.fixed_size=sizeof(XrtManagedFrame),
        .alignment=alignof(XrtManagedFrame), .capacity_offset=offsetof(XrtManagedFrame, slot_count),
        .stride=sizeof(XrtManagedSlot), .drop_payload=frame_dispose};
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
XR_FUNCDEF XrtManagedStatus xrt_managed_root_drain(XrtManagedRoot *root) {
    if (!root_begin(root)) return XRT_MANAGED_ARGUMENT;
    root_clear(root);
    root->busy = false;
    return XRT_MANAGED_OK;
}
XR_FUNCDEF XrtManagedStatus xrt_managed_root_destroy(XrtManagedRoot **owned) {
    if (!owned) return XRT_MANAGED_ARGUMENT;
    if (!*owned) return XRT_MANAGED_OK;
    XrtManagedRoot *root = *owned;
    if (!root_begin(root)) return XRT_MANAGED_ARGUMENT;
    *owned = NULL;
    xrt_managed_drop(&root->header);
    return XRT_MANAGED_OK;
}
XR_FUNCDEF XrtManagedStatus xrt_managed_frame_enter(XrtManagedRoot *root, size_t count,
    XrtManagedFrame **output) {
    if (!output || *output || !count ||
        count > (SIZE_MAX - sizeof(XrtManagedFrame)) / sizeof(XrtManagedSlot) || !root_begin(root))
        return XRT_MANAGED_ARGUMENT;
    XrtManagedPending pending = {0};
    XrtManagedStatus status = xrt_managed_prepare(root->frame_ops,
        sizeof(XrtManagedFrame) + count * sizeof(XrtManagedSlot), &pending);
    if (status == XRT_MANAGED_OK) {
        XrtManagedFrame *frame = (XrtManagedFrame *)pending.value;
        frame->root = root;
        frame->previous = root->top;
        for (size_t i = 0; i < count; ++i) frame_slots(frame)[i] = (XrtManagedSlot){0};
        XrtManagedHeader *committed = NULL;
        status = xrt_managed_commit(&pending, &committed);
        if (status == XRT_MANAGED_OK) root->top = *output = (XrtManagedFrame *)committed;
    }
    xrt_managed_abort(&pending);
    root->busy = false;
    return status;
}
XR_FUNCDEF XrtManagedStatus xrt_managed_frame_leave(XrtManagedRoot *root, XrtManagedFrame *frame) {
    if (!frame || frame->root != root || !root || root->top != frame || !root_begin(root))
        return XRT_MANAGED_ARGUMENT;
    root->top = frame->previous;
    frame->root = NULL;
    frame->previous = NULL;
    xrt_managed_drop(&frame->header);
    root->busy = false;
    return XRT_MANAGED_OK;
}
XR_FUNCDEF XrtManagedStatus xrt_managed_slot_adopt(XrtManagedFrame *frame, size_t index,
    XrtManagedHeader **owned) {
    if (!owned || !*owned) return XRT_MANAGED_ARGUMENT;
    XrtManagedSlot *slot = slot_begin(frame, index);
    if (!slot) return XRT_MANAGED_ARGUMENT;
    XrtManagedStatus status = slot_empty(slot) ? XRT_MANAGED_OK : XRT_MANAGED_ARGUMENT;
    if (status == XRT_MANAGED_OK) { slot->committed = *owned; *owned = NULL; }
    frame->root->busy = false;
    return status;
}
XR_FUNCDEF XrtManagedStatus xrt_managed_slot_borrow(XrtManagedFrame *frame, size_t index,
    XrtManagedHeader **output) {
    if (!output || *output) return XRT_MANAGED_ARGUMENT;
    if (!frame || !frame->root || index >= frame->slot_count) return XRT_MANAGED_ARGUMENT;
    *output = frame_slots(frame)[index].committed;
    return XRT_MANAGED_OK;
}
XR_FUNCDEF XrtManagedStatus xrt_managed_slot_replace(XrtManagedFrame *frame, size_t index,
    XrtManagedHeader **owned) {
    if (!owned || !*owned) return XRT_MANAGED_ARGUMENT;
    XrtManagedSlot *slot = slot_begin(frame, index);
    if (!slot) return XRT_MANAGED_ARGUMENT;
    XrtManagedStatus status = slot->pending.value || slot->pending.allocation_bytes ?
        XRT_MANAGED_ARGUMENT : XRT_MANAGED_OK;
    if (status == XRT_MANAGED_OK) {
        XrtManagedHeader *old = slot->committed;
        slot->committed = *owned;
        *owned = NULL;
        xrt_managed_drop(old);
    }
    frame->root->busy = false;
    return status;
}
XR_FUNCDEF XrtManagedStatus xrt_managed_slot_take(XrtManagedFrame *frame, size_t index,
    XrtManagedHeader **output) {
    if (!output || *output) return XRT_MANAGED_ARGUMENT;
    XrtManagedSlot *slot = slot_begin(frame, index);
    if (!slot) return XRT_MANAGED_ARGUMENT;
    XrtManagedStatus status = !slot->committed || slot->pending.value || slot->pending.allocation_bytes ?
        XRT_MANAGED_ARGUMENT : XRT_MANAGED_OK;
    if (status == XRT_MANAGED_OK) {
        *output = slot->committed;
        slot->committed = NULL;
    }
    frame->root->busy = false;
    return status;
}
XR_FUNCDEF XrtManagedStatus xrt_managed_slot_move(XrtManagedFrame *destination, size_t to,
    XrtManagedFrame *source, size_t from) {
    if (!source || !destination || !source->root || source->root != destination->root ||
        from >= source->slot_count || to >= destination->slot_count) return XRT_MANAGED_ARGUMENT;
    XrtManagedSlot *target = slot_begin(destination, to);
    if (!target) return XRT_MANAGED_ARGUMENT;
    XrtManagedSlot *input = &frame_slots(source)[from];
    XrtManagedStatus status = !slot_empty(target) || !input->committed || input->pending.value ||
        input->pending.allocation_bytes ? XRT_MANAGED_ARGUMENT : XRT_MANAGED_OK;
    if (status == XRT_MANAGED_OK) { target->committed = input->committed; input->committed = NULL; }
    destination->root->busy = false;
    return status;
}
XR_FUNCDEF XrtManagedStatus xrt_managed_slot_drop(XrtManagedFrame *frame, size_t index) {
    XrtManagedSlot *slot = slot_begin(frame, index);
    if (!slot) return XRT_MANAGED_ARGUMENT;
    XrtManagedStatus status = slot->pending.value || slot->pending.allocation_bytes ?
        XRT_MANAGED_ARGUMENT : XRT_MANAGED_OK;
    if (status == XRT_MANAGED_OK) {
        XrtManagedHeader *owned = slot->committed;
        slot->committed = NULL;
        xrt_managed_drop(owned);
    }
    frame->root->busy = false;
    return status;
}
XR_FUNCDEF XrtManagedStatus xrt_managed_slot_prepare(XrtManagedFrame *frame, size_t index,
    XrtManagedOps *ops, size_t bytes, XrtManagedHeader **output) {
    if (!ops || !output || *output) return XRT_MANAGED_ARGUMENT;
    XrtManagedSlot *slot = slot_begin(frame, index);
    if (!slot) return XRT_MANAGED_ARGUMENT;
    XrtManagedStatus status = xrt_managed_prepare(ops, bytes, &slot->pending);
    if (status == XRT_MANAGED_OK) *output = slot->pending.value;
    frame->root->busy = false;
    return status;
}
XR_FUNCDEF XrtManagedStatus xrt_managed_slot_commit(XrtManagedFrame *frame, size_t index) {
    XrtManagedSlot *slot = slot_begin(frame, index);
    if (!slot) return XRT_MANAGED_ARGUMENT;
    XrtManagedHeader *replacement = NULL;
    XrtManagedStatus status = xrt_managed_commit(&slot->pending, &replacement);
    if (status == XRT_MANAGED_OK) {
        XrtManagedHeader *old = slot->committed;
        slot->committed = replacement;
        xrt_managed_drop(old);
    }
    frame->root->busy = false;
    return status;
}
XR_FUNCDEF XrtManagedStatus xrt_managed_slot_abort(XrtManagedFrame *frame, size_t index) {
    XrtManagedSlot *slot = slot_begin(frame, index);
    if (!slot) return XRT_MANAGED_ARGUMENT;
    xrt_managed_abort(&slot->pending);
    frame->root->busy = false;
    return XRT_MANAGED_OK;
}
XR_FUNCDEF XrtManagedStatus xrt_managed_slot_location(XrtManagedFrame *frame, size_t index,
    XrtManagedSlotLocation *output) {
    if (!output || !frame || !frame->root || index >= frame->slot_count) return XRT_MANAGED_ARGUMENT;
    XrtManagedSlot *slot = &frame_slots(frame)[index];
    *output = (XrtManagedSlotLocation){&slot->committed, &slot->pending, &frame->root->busy};
    return XRT_MANAGED_OK;
}
