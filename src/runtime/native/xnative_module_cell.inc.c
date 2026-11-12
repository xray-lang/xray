/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_module_cell.inc.c - Canonical integer Array Cells in actual State slots
 */
static bool module_cell_value(const XrXirValue *value) {
    const XrXirTypeArena *arena = xr_xir_value_arena(value);
    return value && value->type == XR_XIR_CONSTRUCTED_TYPE_BASE + 4 &&
        xir_native_type_arena_i64_views(arena) && !xr_xir_cell_is_projection(value) &&
        xr_xir_value_argument(value, arena, (XrXirType)value->type);
}
static XrXirValueStatus module_cell_copy(const XrtModuleType *type, const void *source, void *empty) {
    (void)type;
    return module_cell_value(source) ? xr_xir_value_copy(source, empty) : XR_XIR_VALUE_BAD_ARGUMENT;
}
static void module_cell_drop(const XrtModuleType *type, void *value) {
    (void)type;
    xr_xir_value_drop(value);
}
XR_FUNC XrXirValueStatus xrt_module_cell_construct(XrtModuleState *state, uint32_t slot,
    const XrXirValue *value, XrtModuleRef **output) {
    if (!state || slot >= state->layout->view.slot_count || !module_cell_value(value) ||
        module_slot_type(state, slot)->kind != XRT_MODULE_CELL_ARRAY_I64) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XirObject *cell = (const XirObject *)(intptr_t)value->payload;
    XrXirCellPublication publication = {state, cell->domain, slot};
    XrXirValueStatus status = xr_xir_cell_publication_prepare(value, &publication);
    if (status != XR_XIR_VALUE_OK) return status;
    status = module_construct_value(state, slot, value, output);
    if (status == XR_XIR_VALUE_OK) xr_xir_cell_publication_commit(value, &publication);
    return status;
}
XR_FUNC XrXirValueStatus xrt_module_cell_borrow(const XrtModuleRef *reference,
    XrXirValue *empty, XrXirCellPublication *publication, bool *writable) {
    if (!empty || empty->type || empty->reserved || empty->payload || !publication || !writable ||
        !module_reference_valid(reference)) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrtModuleState *state = reference->state;
    uint32_t slot = reference->slot;
    if (module_slot_type(state, slot)->kind != XRT_MODULE_CELL_ARRAY_I64 ||
        (state->flags[slot] & reference->required_flags) != reference->required_flags)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirValue *value = module_payload(state, slot);
    if (!module_cell_value(value)) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XirObject *cell = (const XirObject *)(intptr_t)value->payload;
    XrXirCellPublication actual = {state, cell->domain, slot};
    if (!xr_xir_cell_publication_matches(value, &actual)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueStatus status = xr_xir_value_copy(value, empty);
    if (status == XR_XIR_VALUE_OK) {
        *publication = actual;
        *writable = (state->layout->view.slots[slot].flags & XRT_MODULE_SLOT_MUTABLE) != 0;
    }
    return status;
}
