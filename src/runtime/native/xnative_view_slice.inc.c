/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_view_slice.inc.c - Logical bindings on actual canonical view scopes
 */
typedef struct XrtViewAccessContext { XrtViewFrameBody *frame; XrtViewScope *scope; } XrtViewAccessContext;

static XrXirValueStatus view_authorize(void *user, const XrXirValue *value,
    bool writable, XrXirValueAdmission *admission, XrXirCellAuthority *output) {
    XrtViewAccessContext *context = user;
    XrtViewScope *scope = context->scope;
    if (writable || !scope || !scope->active || !scope->pin.body ||
        admission->arena != xr_xir_value_arena(value) ||
        !view_frame_ancestor(context->frame, scope->owner) ||
        (void *)(intptr_t)value->payload != scope->pin.body) return XR_XIR_VALUE_BAD_ARGUMENT;
    *output = scope->authority;
    return XR_XIR_VALUE_OK;
}
static XrtViewSlot *view_slice(XrtViewFrameBody *frame, XrtSliceI64 token, uint32_t source) {
    XrtViewRecord record = view_require(frame, token.identity, XRT_VIEW_SLICE, source);
    XrtViewSlot *slot = record.slot == UINT32_MAX ? &record.frame->result : &record.frame->views[record.slot];
    if (slot->identity != token.identity || !slot->scope || !slot->scope->active)
        xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    return slot;
}
static XrtSliceI64 view_install(XrtViewFrameBody *frame, uint32_t ordinal,
    uint64_t identity, XrtViewScope *scope) {
    view_clear_slot(frame, ordinal);
    frame->views[ordinal] = (XrtViewSlot){frame->pending, scope, identity};
    frame->pending = (XrXirValue){0};
    view_record_insert(frame->registry, (XrtViewRecord){identity, frame, ordinal, XRT_VIEW_SLICE});
    return (XrtSliceI64){identity};
}
static XrtViewScope *view_scope_prepare(XrtViewFrameBody *frame, uint64_t identity, uint32_t source) {
    XrtViewScope *scope = view_allocate(frame->registry, sizeof(*scope), source);
    frame->pending_scope = scope;
    scope->owner = frame;
    scope->authority = (XrXirCellAuthority){frame->registry, scope, identity};
    return scope;
}
static void view_scope_publish(XrtViewFrameBody *frame, XrtViewScope *scope) {
    xr_xir_view_scope_pin_created(&frame->pending, &scope->authority, &scope->pin);
    scope->references = 1;
    scope->active = true;
    scope->next = frame->scopes;
    if (scope->next) scope->next->previous = scope;
    frame->scopes = scope;
    frame->pending_scope = NULL;
}
static int64_t view_range_length(XrtViewRange range, int64_t length, uint32_t source) {
    if (range.open_end) range.end = length;
    if (range.start < 0 || range.end < range.start || range.end > length)
        xrt_value_failure(XR_XIR_VALUE_BOUNDS, source);
    return range.end - range.start;
}
static XrtViewRange view_reborrow_range(XrtViewRange range, int64_t length) {
    if (range.start < 0) range.start += length;
    if (range.start < 0) range.start = 0;
    if (range.start > length) range.start = length;
    if (range.open_end) range.end = length;
    else if (range.end < 0) range.end += length;
    if (range.end < 0) range.end = 0;
    if (range.end > length) range.end = length;
    if (range.end < range.start) range.start = range.end;
    range.open_end = false;
    return range;
}
XRAY_API XR_NOINLINE XrtSliceI64 xrt_slice_i64_new(XrtViewFrame token, uint32_t ordinal,
    XrtViewCell cell, XrtViewRange range, uint32_t source) {
    XrtViewFrameBody *frame = view_current(token, source);
    if (ordinal >= frame->view_count) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    XrtViewRecord origin = view_require(frame, cell.identity, XRT_VIEW_CELL, source);
    XrtViewCellSlot *root = &origin.frame->cells[origin.slot];
    XrXirValueAdmission admission = view_value_admission(&root->value);
    view_require_status(xr_xir_cell_authorized_read(&root->value, view_cell_authority(root), &frame->pending), source);
    int64_t length = 0;
    view_require_status(xr_xir_array_len(&frame->pending, &admission, &length), source);
    xr_xir_value_drop(&frame->pending);
    length = view_range_length(range, length, source);
    uint64_t identity = view_identity(source);
    XrtViewScope *scope = view_scope_prepare(frame, identity, source);
    scope->module_root = xr_xir_cell_module_storage(&root->value);
    scope->root_owner = scope->module_root ? NULL : origin.frame;
    XrXirViewStorageSpec spec = {scope->authority, range.start, length, XR_XIR_TYPE_SLICE};
    view_require_status(xr_xir_view_new(VIEW_SLICE_TYPE, &root->value, NULL, &spec,
        &admission, &frame->pending), source);
    view_scope_publish(frame, scope);
    return view_install(frame, ordinal, identity, scope);
}
XRAY_API XR_NOINLINE XrtSliceI64 xrt_slice_i64_reborrow(XrtViewFrame token, uint32_t ordinal,
    XrtSliceI64 input, XrtViewRange range, uint32_t source) {
    XrtViewFrameBody *frame = view_current(token, source);
    if (ordinal >= frame->view_count) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    XrtViewSlot *parent = view_slice(frame, input, source);
    if (parent->scope->references == UINT32_MAX) xrt_value_failure(XR_XIR_VALUE_REFCOUNT_LIMIT, source);
    XrXirValueAdmission admission = view_value_admission(&parent->value);
    XrtViewAccessContext context = {frame, parent->scope};
    XrXirViewAccess access = {&admission, view_authorize, &context};
    int64_t length = 0;
    view_require_status(xr_xir_view_len(&parent->value, &access, &length), source);
    range = view_reborrow_range(range, length);
    length = view_range_length(range, length, source);
    uint64_t identity = view_identity(source);
    XrtViewScope *scope = view_scope_prepare(frame, identity, source);
    XrXirViewStorageSpec spec = {scope->authority, range.start, length, XR_XIR_TYPE_SLICE};
    view_require_status(xr_xir_view_reborrow(VIEW_SLICE_TYPE, &parent->value, &spec,
        &access, &frame->pending), source);
    scope->parent = parent->scope;
    scope->root_owner = parent->scope->root_owner;
    scope->module_root = parent->scope->module_root;
    ++scope->parent->references;
    view_scope_publish(frame, scope);
    return view_install(frame, ordinal, identity, scope);
}
XRAY_API XR_NOINLINE XrtSliceI64 xrt_slice_i64_bind(XrtViewFrame token, uint32_t ordinal,
    XrtSliceI64 input, uint32_t source) {
    XrtViewFrameBody *frame = view_current(token, source);
    if (ordinal >= frame->view_count) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    XrtViewSlot *value = view_slice(frame, input, source);
    XrtViewScope *scope = value->scope;
    if (scope->references == UINT32_MAX) xrt_value_failure(XR_XIR_VALUE_REFCOUNT_LIMIT, source);
    uint64_t identity = view_identity(source);
    view_require_status(xr_xir_value_copy(&value->value, &frame->pending), source);
    ++scope->references;
    if (frame->result.identity == input.identity) view_clear_slot(frame, UINT32_MAX);
    return view_install(frame, ordinal, identity, scope);
}
XRAY_API XR_NOINLINE int64_t xrt_slice_i64_length(XrtViewFrame token, XrtSliceI64 input, uint32_t source) {
    XrtViewFrameBody *frame = view_current(token, source);
    XrtViewSlot *value = view_slice(frame, input, source);
    XrXirValueAdmission admission = view_value_admission(&value->value);
    XrtViewAccessContext context = {frame, value->scope};
    XrXirViewAccess access = {&admission, view_authorize, &context};
    int64_t length = 0;
    view_require_status(xr_xir_view_len(&value->value, &access, &length), source);
    return length;
}
XRAY_API XR_NOINLINE int64_t xrt_slice_i64_get(XrtViewFrame token, XrtSliceI64 input,
    int64_t index, uint32_t source) {
    XrtViewFrameBody *frame = view_current(token, source);
    XrtViewSlot *value = view_slice(frame, input, source);
    XrXirValueAdmission admission = view_value_admission(&value->value);
    XrtViewAccessContext context = {frame, value->scope};
    XrXirViewAccess access = {&admission, view_authorize, &context};
    XrXirValue result = {0};
    XrXirFaultDetail fault = {0};
    view_require_status(xr_xir_view_get(&value->value, index, &access, &result, &fault), source);
    XR_CHECK(result.type == XR_XIR_I64, "the closed Slice element has its admitted scalar representation");
    return result.payload;
}
XRAY_API XR_NOINLINE XrtArray *xrt_slice_i64_copy(XrtViewFrame token, XrtSliceI64 input, uint32_t source) {
    XrtViewFrameBody *frame = view_current(token, source);
    XrtViewSlot *value = view_slice(frame, input, source);
    XrXirValueAdmission admission = view_value_admission(&value->value);
    XrtViewAccessContext context = {frame, value->scope};
    XrXirViewAccess access = {&admission, view_authorize, &context};
    view_require_status(xr_xir_view_copy(VIEW_ARRAY_TYPE, &value->value, &access,
        &frame->registry->root->pending), source);
    return xrt_array_publish_pending(source);
}
XRAY_API XR_NOINLINE void xrt_slice_i64_end(XrtViewFrame token, XrtSliceI64 input, uint32_t source) {
    XrtViewFrameBody *frame = view_current(token, source);
    XrtViewRecord record = view_require(frame, input.identity, XRT_VIEW_SLICE, source);
    if (record.frame != frame) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    view_clear_slot(frame, record.slot);
}
XRAY_API XR_NOINLINE void xrt_slice_i64_cleanup(XrtViewFrame token, uint32_t ordinal, uint32_t source) {
    XrtViewFrameBody *frame = view_current(token, source);
    if (ordinal >= frame->view_count) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    view_clear_slot(frame, ordinal);
}
XRAY_API XR_NOINLINE XrtSliceI64 xrt_slice_i64_return(XrtViewFrame token,
    XrtSliceI64 input, uint32_t source) {
    XrtViewFrameBody *frame = view_current(token, source);
    XrtViewRecord origin = view_require(frame, input.identity, XRT_VIEW_SLICE, source);
    XrtViewSlot *slot = view_slice(frame, input, source);
    XrtViewScope *scope = slot->scope;
    XrtViewFrameBody *parent = frame->parent;
    if (origin.frame != frame || !parent || parent->result.identity ||
        (!scope->module_root && (!scope->root_owner || scope->root_owner == frame ||
            !view_frame_ancestor(parent, scope->root_owner)))) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    uint64_t identity = view_identity(source);
    for (XrtViewScope *retained = scope; retained && retained->owner == frame; retained = retained->parent) {
        view_scope_unlink(retained);
        retained->owner = parent;
        retained->next = parent->scopes;
        if (retained->next) retained->next->previous = retained;
        parent->scopes = retained;
    }
    view_record_remove(frame->registry, slot->identity);
    parent->result = *slot;
    parent->result.identity = identity;
    *slot = (XrtViewSlot){0};
    view_record_insert(frame->registry, (XrtViewRecord){identity, parent, UINT32_MAX, XRT_VIEW_SLICE});
    return (XrtSliceI64){identity};
}
