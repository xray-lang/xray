/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_string_append.inc.c - Publish actual bindings before retiring owners
 */
static XrXirValueAdmission string_append_admission(XrtValueRoot *root, uint32_t source) {
    return (XrXirValueAdmission){NULL, root_domain(root, source), NULL, NULL, UINT64_MAX, UINT64_MAX};
}

static XR_AINLINE bool root_append_in_place(XrtValueRoot *root, XrtString *previous,
    const XrtString *suffix, uint32_t source) {
    root_borrow(root, suffix);
    XrtStringOwner *owner = owner_find(root, previous);
    if (!owner || !owner->count) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    XR_CHECK(!root->pending.type && !root->pending.reserved && !root->pending.payload,
        "String append reserves one independent pending owner");
    return owner->count == 1 && !owner->pinned &&
        xr_xir_root_string_append_in_place(root->domain, previous, suffix);
}
static XR_NOINLINE void string_cell_append_transaction(XrtValueRoot *root, XrtString **binding,
    const XrtString *suffix, uint32_t source) {
    root_reserve(root, source);
    XrtString *previous = *binding;
    XrtStringOwner *owner = owner_find(root, previous);
    if (!owner || !owner->count) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    XR_CHECK(!root->pending.type && !root->pending.reserved && !root->pending.payload,
        "String append reserves one independent pending owner");
    /* The canonical grow operation may detach the inline body. Escaping once
     * moves a private Root block to its existing shared allocation ledger. */
    if (previous->object.root_private) {
        XrXirValueStatus escaped = xr_xir_root_string_escape(previous);
        if (escaped) xrt_value_failure(escaped, source);
    }
    bool copied = owner->count != 1 || owner->pinned;
    XrXirValue current = string_value(previous), retired = {0};
    if (copied) {
        XrXirValueStatus status = xr_xir_value_copy(&current, &root->pending);
        if (status) xrt_value_failure(status, source);
        current = root->pending;
    }
    XrXirValue incoming = string_value(suffix);
    XrXirValueAdmission admission = string_append_admission(root, source);
    XrXirValuePlace place = {XR_XIR_STRING, &current.payload, NULL};
    /* Graph draining waits for the binding and registry to agree. Releasing
     * retired storage may invoke its callback synchronously after that commit. */
    xr_xir_value_graph_begin();
    XrXirValueStatus status = xr_xir_string_place_append_deferred(&place, &incoming, &admission, &retired);
    if (status) {
        if (copied) xr_xir_value_drop(&root->pending);
        xr_xir_value_graph_end();
        xrt_value_failure(status, source);
    }
    XrtString *result = (XrtString *)(intptr_t)current.payload;
    if (copied && result == previous) {
        xr_xir_value_drop(&root->pending);
        xr_xir_value_graph_end();
        return;
    }
    if (copied || result != previous) {
        owner = owner_find(root, previous);
        XR_CHECK(owner && owner->count, "The append transaction retains its original guest owner");
        if (copied) {
            --owner->count;
            --root->held;
        } else {
            owner_remove(root, owner);
            --root->held;
        }
        root_adopt(root, result, false);
        *binding = result;
        root->pending = (XrXirValue){0};
    }
    xr_xir_value_drop(&retired);
    xr_xir_value_graph_end();
}

/* Only the cold path exposes this temporary binding's address. The ordinary
 * owned entry needs no transaction frame or address-exposed local. */
static XR_NOINLINE XrtString *string_owned_append_transaction(XrtValueRoot *root,
    XrtString *current, const XrtString *suffix, uint32_t source) {
    string_cell_append_transaction(root, &current, suffix, source);
    return current;
}
XRAY_API XR_NOINLINE void xrt_string_cell_append(XrtString **binding, const XrtString *suffix,
    uint32_t source) {
    XrtValueRoot *root = string_root(source);
    if (!binding || !*binding) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    if (!root_append_in_place(root, *binding, suffix, source))
        string_cell_append_transaction(root, binding, suffix, source);
}
XRAY_API XR_NOINLINE XrtString *xrt_string_append_owned(XrtString *current,
    const XrtString *suffix, uint32_t source) {
    XrtValueRoot *root = string_root(source);
    if (!current) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    if (!root_append_in_place(root, current, suffix, source))
        return string_owned_append_transaction(root, current, suffix, source);
    return current;
}

XRAY_API XR_NOINLINE XrtString *xrt_string_append_cursor_refill(XrtStringAppendCursor *cursor,
    XrtString *current, const XrtString *suffix, uint32_t source) {
    if (!cursor) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    xrt_string_append_cursor_close(cursor);
    *cursor = (XrtStringAppendCursor){0};
    XrtString *result = xrt_string_append_owned(current, suffix, source);
    XrtValueRoot *root = string_root(source);
    XrtStringOwner *owner = owner_find(root, result);
    XrtStringOwner *literal = owner_find(root, suffix);
    if (owner && owner->count == 1 && !owner->pinned && literal && literal->pinned &&
        xr_xir_root_string_append_cursor(root->domain, result, &cursor->cursor)) {
        cursor->string = result;
        cursor->suffix = suffix;
    }
    return result;
}

static XrtString *string_ref_current(XrtStringRef reference, uint32_t source) {
    if (reference.cell) {
        if (reference.state.reference) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
        return *reference.cell;
    }
    XrtModuleAccess state = reference.state;
    if (!state.reference || !state.flags || !state.payload || !state.is_mutable ||
        state.kind != XRT_MODULE_STRING || state.size != sizeof(XrtString *))
        xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    if ((*state.flags & state.required_flags) != state.required_flags) xrt_module_uninitialized(source);
    XrtString *current = NULL;
    memcpy(&current, state.payload, sizeof(current));
    return current;
}

XRAY_API XrtStringRef xrt_string_ref_local(XrtString **cell, uint32_t source) {
    if (!cell || !*cell) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    root_borrow(string_root(source), *cell);
    return (XrtStringRef){.cell=cell};
}
XRAY_API XrtStringRef xrt_string_ref_state(XrtModuleAccess state, uint32_t source) {
    if (!state.reference || !state.payload || !state.flags || !state.is_mutable ||
        state.kind != XRT_MODULE_STRING || state.size != sizeof(XrtString *))
        xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    return (XrtStringRef){.state=state};
}
XRAY_API XrtString *xrt_string_ref_read(XrtStringRef reference, uint32_t source) {
    return xrt_string_retain(string_ref_current(reference, source), source);
}
XRAY_API void xrt_string_ref_write(XrtStringRef reference, const XrtString *value, uint32_t source) {
    (void)string_ref_current(reference, source);
    if (reference.cell) {
        XrtString *previous = *reference.cell;
        XrtString *prepared = xrt_string_retain(value, source);
        *reference.cell = prepared;
        xrt_string_drop(&previous);
    } else {
        XrtString *prepared = NULL;
        XrXirValueStatus status = xrt_string_owned_copy(value, &prepared);
        if (status) xrt_value_failure(status, source);
        XrtString *previous = NULL;
        memcpy(&previous, reference.state.payload, sizeof(previous));
        memcpy(reference.state.payload, &prepared, sizeof(prepared));
        xrt_string_owned_drop(&previous);
    }
}
XRAY_API XR_NOINLINE void xrt_string_ref_append(XrtStringRef reference, const XrtString *suffix,
    uint32_t source) {
    XrtString *previous = string_ref_current(reference, source);
    if (reference.cell) { xrt_string_cell_append(reference.cell, suffix, source); return; }
    XrtValueRoot *root = string_root(source);
    root_borrow(root, suffix);
    XrXirValue current = string_value(previous), incoming = string_value(suffix), retired = {0};
    XrXirValueAdmission admission = string_append_admission(root, source);
    XrXirValuePlace place = {XR_XIR_STRING, &current.payload, NULL};
    xr_xir_value_graph_begin();
    XrXirValueStatus status = xr_xir_string_place_append_deferred(&place, &incoming, &admission, &retired);
    if (status) {
        xr_xir_value_graph_end();
        xrt_value_failure(status, source);
    }
    XrtString *result = (XrtString *)(intptr_t)current.payload;
    memcpy(reference.state.payload, &result, sizeof(result));
    xr_xir_value_drop(&retired);
    xr_xir_value_graph_end();
}
