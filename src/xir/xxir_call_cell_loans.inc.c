/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_call_cell_loans.inc.c - Checked cell roles and inline frame loan ownership
 *
 * KEY CONCEPT:
 *   Complete preparation precedes loan publication and every pop restores loans.
 */
static bool call_cell_work(XrXirCall *call, uint64_t work) {
    XrXirValueAdmission *admission = &call->config.admission;
    if (work > admission->work) return false;
    admission->work -= work;
    return !admission->domain || xr_xir_domain_work(admission->domain, work);
}
static XrXirCellAuthority frame_cell_authority(const XrXirCall *call, const CallFrame *frame) {
    return frame ? (XrXirCellAuthority){call, frame, frame->epoch} : (XrXirCellAuthority){0};
}
static bool view_cell_authority(const XrXirCallView *view, XrXirCellAuthority *output) {
    if (!xr_xir_call_admission(view) || xr_xir_call_execution_status(view) != XR_XIR_CALL_READY) return false;
    *output = frame_cell_authority(view->activation, view->activation->top);
    return true;
}
XR_FUNC XrXirValueStatus xr_xir_call_cell_read(const XrXirCallView *view,
    const XrXirValue *cell, XrXirValue *output) {
    XrXirCellAuthority authority = {0};
    if (!view_cell_authority(view, &authority)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!call_cell_work(view->activation, 1)) return XR_XIR_VALUE_LIMIT;
    if (xr_xir_cell_is_projection(cell)) {
        XrXirValuePath path = {0}; XrXirFaultDetail fault = {0};
        return xr_xir_cell_authorized_path_read(cell, &authority, &path,
            xr_xir_call_admission(view), output, &fault);
    }
    return xr_xir_cell_authorized_read(cell, &authority, output);
}
XR_FUNC XrXirValueStatus xr_xir_call_cell_write(const XrXirCallView *view,
    const XrXirValue *cell, const XrXirValue *value) {
    XrXirCellAuthority authority = {0};
    if (!view_cell_authority(view, &authority)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!call_cell_work(view->activation, 1)) return XR_XIR_VALUE_LIMIT;
    return xr_xir_cell_authorized_write(cell, &authority, value, xr_xir_call_admission(view));
}
XR_FUNC XrXirValueStatus xr_xir_call_cell_place(const XrXirCallView *view,
    const XrXirValue *cell, XrXirValuePlace *output) {
    XrXirCellAuthority authority = {0};
    if (!view_cell_authority(view, &authority)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!call_cell_work(view->activation, 1)) return XR_XIR_VALUE_LIMIT;
    return xr_xir_cell_authorized_place(cell, &authority, xr_xir_call_admission(view), output);
}
XR_FUNC XrXirValueStatus xr_xir_call_cell_project(const XrXirCallView *view,
    XrXirType type, const XrXirValue *root, const XrXirValuePath *path, XrXirValue *output) {
    XrXirCellAuthority authority = {0};
    if (!view_cell_authority(view, &authority)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!call_cell_work(view->activation, 1)) return XR_XIR_VALUE_LIMIT;
    return xr_xir_cell_project_authorized(type, root, path, &authority,
        xr_xir_call_admission(view), output);
}
XR_FUNC XrXirValueStatus xr_xir_call_cell_path_read(const XrXirCallView *view,
    const XrXirValue *cell, const XrXirValuePath *path,
    XrXirValue *output, XrXirFaultDetail *fault) {
    XrXirCellAuthority authority = {0};
    if (!view_cell_authority(view, &authority)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!call_cell_work(view->activation, 1)) return XR_XIR_VALUE_LIMIT;
    return xr_xir_cell_authorized_path_read(cell, &authority, path,
        xr_xir_call_admission(view), output, fault);
}
XR_FUNC XrXirValueStatus xr_xir_call_cell_path_write(const XrXirCallView *view,
    const XrXirValue *cell, const XrXirValuePath *path,
    const XrXirValue *value, XrXirFaultDetail *fault, bool append) {
    XrXirCellAuthority authority = {0};
    if (!view_cell_authority(view, &authority)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!call_cell_work(view->activation, 1)) return XR_XIR_VALUE_LIMIT;
    XrXirValueAdmission *admission = xr_xir_call_admission(view);
    return append ? xr_xir_cell_authorized_path_push(cell, &authority, path, value, admission, fault) :
        xr_xir_cell_authorized_path_write(cell, &authority, path, value, admission, fault);
}
static uint32_t frame_cell_count(const XrXirCall *call, const XrXirCallEntry *entry) {
    const XrXirTypes *types = xr_xir_compile_type_arena_types(call->config.admission.arena);
    uint32_t count = 0;
    for (uint32_t p = 0; p < entry->parameter_count; ++p)
        if (xr_xir_type_is_cell(types, entry->parameters[p])) ++count;
    return count;
}
static XrXirCallStatus frame_cell_prepare(XrXirCall *call, CallFrame *frame,
    uint32_t captures, XrXirFaultDetail *fault) {
    if (!frame->cell_capacity) return XR_XIR_CALL_READY;
    const XrXirTypes *types = xr_xir_compile_type_arena_types(call->config.admission.arena);
    uint32_t entry = (uint32_t)(frame->entry - call->config.entries), count = 0;
    XrXirCellAuthority parent = frame_cell_authority(call, frame->parent);
    for (uint32_t p = 0; p < frame->entry->parameter_count; ++p) {
        if (!call_cell_work(call, 1)) return XR_XIR_CALL_LIMIT;
        if (!xr_xir_type_is_cell(types, frame->entry->parameters[p])) continue;
        if (!call->cell_role) return XR_XIR_CALL_BAD_ARGUMENT;
        XrXirCellParameterRole role = call->cell_role(call->cell_context, entry, p);
        const XrXirValue *cell = &frame->arguments[p];
        if (role == XR_XIR_CELL_ROLE_OWNED_CAPTURE) {
            if (p >= captures || !xr_xir_cell_unborrowed(cell) || xr_xir_cell_module_storage(cell))
                return XR_XIR_CALL_BAD_ARGUMENT;
            continue;
        }
        if (p < captures || (role != XR_XIR_CELL_ROLE_SCOPED_REF && role != XR_XIR_CELL_ROLE_LEXICAL_CLEANUP))
            return XR_XIR_CALL_BAD_ARGUMENT;
        if (role == XR_XIR_CELL_ROLE_LEXICAL_CLEANUP) {
            if (!frame->parent || frame->entry->cleanup_owner !=
                    (uint32_t)(frame->parent->entry - call->config.entries) + 1 || !frame->parent->exiting)
                return XR_XIR_CALL_BAD_ARGUMENT;
        } else if (frame->entry->cleanup_owner) return XR_XIR_CALL_BAD_ARGUMENT;
        for (uint32_t prior = 0; prior < p; ++prior) {
            if (!call_cell_work(call, 1)) return XR_XIR_CALL_LIMIT;
            if (xr_xir_type_is_cell(types, frame->entry->parameters[prior]) &&
                xr_xir_cell_same_owner(cell, &frame->arguments[prior])) return XR_XIR_CALL_BAD_ARGUMENT;
        }
        XrXirValueStatus prepared = xr_xir_cell_loan_prepare_path(cell, &parent,
            &call->config.admission, fault);
        if (prepared != XR_XIR_VALUE_OK) return prepared == XR_XIR_VALUE_OOM ? XR_XIR_CALL_OOM :
            prepared == XR_XIR_VALUE_LIMIT || prepared == XR_XIR_VALUE_REFCOUNT_LIMIT ? XR_XIR_CALL_LIMIT :
            prepared == XR_XIR_VALUE_BOUNDS && xr_xir_fault_bounds_valid(*fault) ?
                XR_XIR_CALL_BOUNDS : XR_XIR_CALL_BAD_ARGUMENT;
        /* Store ordinal before publication; commit replaces it with the live owner. */
        XR_CHECK(count < frame->cell_capacity, "frame reserves every possible cell loan before preparation");
        frame->loans[count++].parameter = p;
    }
    frame->loan_count = count;
    return XR_XIR_CALL_READY;
}
static void frame_cell_commit(XrXirCall *call, CallFrame *frame) {
    XrXirCellAuthority authority = frame_cell_authority(call, frame);
    for (uint32_t i = 0; i < frame->loan_count; ++i) {
        CallCellLoan *record = &frame->loans[i];
        xr_xir_cell_loan_commit(&frame->arguments[record->parameter], &record->loan, &authority);
    }
}
static void frame_cell_release(CallFrame *frame) {
    while (frame->loan_count) xr_xir_cell_loan_release(&frame->loans[--frame->loan_count].loan);
}
