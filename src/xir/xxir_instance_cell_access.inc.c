/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_cell_access.inc.c - Actual module owner admission
 *
 * KEY CONCEPT:
 *   A frame loan does not grant worker access to module storage.
 */
static bool instance_cell_work(XrXirCallView *view, uint64_t work) {
    XrXirValueAdmission *admission = xr_xir_call_admission(view);
    if (!admission || work > admission->work) return false;
    admission->work -= work;
    return admission->domain && xr_xir_domain_work(admission->domain, work);
}
static bool instance_cell_published(const XrXirInstance *instance, uint32_t slot,
    const XrXirValue *cell) {
    const XrXirDeclarations *d = instance->program->declarations;
    if (slot >= d->slot_count || !d->slots[slot].mutable || !instance->published[slot]) return false;
    XrXirCellPublication publication = {instance, instance->domain, slot};
    return xr_xir_cell_publication_matches(cell, &publication) &&
        xr_xir_cell_same_owner(cell, &instance->slots[slot]);
}
static XrXirCallStatus instance_cell_access(XrXirCallView *view,
    const XrXirInstance *instance, const XrXirValue *cell) {
    if (!xr_xir_cell_in_domain(cell, instance->domain)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (!xr_xir_cell_module_storage(cell)) return XR_XIR_CALL_READY;
    if (view_is_child(instance, view)) return XR_XIR_CALL_BAD_STATE;
    const XrXirDeclarations *d = instance->program->declarations;
    for (uint32_t slot = 0; slot < d->slot_count; ++slot) {
        if (!instance_cell_work(view, 1)) return XR_XIR_CALL_LIMIT;
        if (!instance_cell_published(instance, slot, cell)) continue;
        uint32_t module = d->slots[slot].module;
        return instance->ready[module] || instance->current_module == module ?
            XR_XIR_CALL_READY : XR_XIR_CALL_BAD_STATE;
    }
    return XR_XIR_CALL_BAD_STATE;
}
