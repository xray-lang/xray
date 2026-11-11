/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_state.inc.c - Copying state layouts and immutable capability queries
 *
 * KEY CONCEPT:
 *   Layout storage is committed only after all input arrays are copied.
 *   Reference shapes belong to SSA values inside the same module owner.
 */
XR_FUNC bool xr_lir_internal_state_payload(XrLirType type, uint32_t *size, uint32_t *alignment) {
    switch (type) {
    case XR_LIR_UNIT: *size = 0; *alignment = 1; return true;
    case XR_LIR_I1: case XR_LIR_I8: case XR_LIR_U8: *size = 1; *alignment = 1; return true;
    case XR_LIR_I16: case XR_LIR_U16: *size = 2; *alignment = 2; return true;
    case XR_LIR_I32: case XR_LIR_U32: case XR_LIR_F32: *size = 4; *alignment = 4; return true;
    case XR_LIR_I64: case XR_LIR_U64: case XR_LIR_F64: *size = 8; *alignment = 8; return true;
    case XR_LIR_STRING: case XR_LIR_ARRAY:
        *size = sizeof(void *); *alignment = _Alignof(void *); return true;
    default: return false;
    }
}

static void lir_state_layout_free(XrLirStateLayout *layout) {
    xr_free((void *)layout->modules);
    xr_free((void *)layout->dependencies);
    xr_free((void *)layout->slots);
    *layout = (XrLirStateLayout){0};
}

static bool lir_state_layout_shape(const XrLirStateLayout *layout) {
    if (!layout || !layout->module_count || !layout->modules ||
        (layout->dependency_count != 0) != (layout->dependencies != NULL) ||
        (layout->slot_count != 0) != (layout->slots != NULL) ||
        !layout->payload_alignment || (layout->payload_alignment & (layout->payload_alignment - 1)) ||
        layout->payload_size > SIZE_MAX || layout->payload_size % layout->payload_alignment)
        return false;
    uint32_t dependency = 0;
    for (uint32_t m = 0; m < layout->module_count; ++m) {
        XrLirStateModule module = layout->modules[m];
        if (module.first_dependency != dependency ||
            module.dependency_count > layout->dependency_count - dependency) return false;
        for (uint32_t d = 0; d < module.dependency_count; ++d) {
            uint32_t target = layout->dependencies[dependency + d];
            if (target >= layout->module_count || target == m) return false;
        }
        dependency += module.dependency_count;
    }
    if (dependency != layout->dependency_count) return false;
    uint64_t end = 0;
    for (uint32_t s = 0; s < layout->slot_count; ++s) {
        const XrLirStateSlot *slot = &layout->slots[s];
        uint32_t size, alignment;
        if (slot->module >= layout->module_count ||
            (slot->payload == XR_LIR_ARRAY ? !xr_lir_internal_array_element(slot->array_element) :
                slot->array_element != XR_LIR_TYPE_INVALID) ||
            !xr_lir_internal_state_payload(slot->payload, &size, &alignment) ||
            slot->size != size || slot->alignment != alignment ||
            alignment > layout->payload_alignment || slot->offset % alignment ||
            slot->offset < end || slot->offset > layout->payload_size ||
            size > layout->payload_size - slot->offset) return false;
        end = slot->offset + size;
    }
    return true;
}

XR_FUNC bool xr_lir_internal_state_layout_valid(const XrLirStateLayout *layout) {
    return lir_state_layout_shape(layout);
}

XR_FUNC XrLirStatus xr_lir_set_state_layout(XrLirModule *m, const XrLirStateLayout *input) {
    XrLirStatus status = lir_writable(m);
    if (status != XR_LIR_OK) return status;
    if (m->state_layout_set || !lir_state_layout_shape(input))
        return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
    XrLirStateLayout copy = *input;
    copy.modules = lir_copy(m, input->modules, input->module_count, sizeof(*copy.modules));
    copy.dependencies = lir_copy(m, input->dependencies, input->dependency_count, sizeof(*copy.dependencies));
    copy.slots = lir_copy(m, input->slots, input->slot_count, sizeof(*copy.slots));
    if (!copy.modules || (copy.dependency_count && !copy.dependencies) ||
        (copy.slot_count && !copy.slots)) {
        lir_state_layout_free(&copy);
        return m->failure;
    }
    m->state_layout = copy;
    m->state_layout_set = true;
    return XR_LIR_OK;
}

XR_FUNC XrLirStatus xr_lir_set_state_entry(XrLirModule *m, XrLirFunction function) {
    XrLirStatus status = lir_writable(m);
    if (status != XR_LIR_OK) return status;
    XrLirFunctionData *f = lir_function(m, function);
    if (!m->state_layout_set || m->state_entry || !f ||
        f->kind != XR_LIR_FUNCTION_DEFINITION || !f->parameter_count ||
        f->parameter_types[f->parameter_count - 1] != XR_LIR_STATE)
        return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
    m->state_entry = function.id;
    return XR_LIR_OK;
}

XR_FUNC XrLirStatus xr_lir_set_state_reference(XrLirModule *m, XrLirValue value,
    const XrLirStateReference *shape) {
    XrLirStatus status = lir_writable(m);
    if (status != XR_LIR_OK) return status;
    XrLirFunctionData *f = lir_function(m, (XrLirFunction){value.module, value.function});
    uint32_t size, alignment;
    if (!f || !value.id || value.id > f->value_count || !shape ||
        (shape->payload == XR_LIR_ARRAY ? !xr_lir_internal_array_element(shape->array_element) :
            shape->array_element != XR_LIR_TYPE_INVALID) ||
        !xr_lir_internal_state_payload(shape->payload, &size, &alignment))
        return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
    XrLirValueData *v = &f->values[value.id - 1];
    if (!m->state_layout_set || v->type != XR_LIR_STATE_REF || !v->parameter || !v->block ||
        v->state_reference.payload != XR_LIR_TYPE_INVALID)
        return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
    v->state_reference = *shape;
    return XR_LIR_OK;
}

XR_FUNC XrLirStatus xr_lir_state_layout_view(const XrLirModule *m, XrLirStateLayout *out) {
    if (out) *out = (XrLirStateLayout){0};
    if (!out || !xr_lir_module_is_readable(m)) return XR_LIR_INVALID_ARGUMENT;
    *out = m->state_layout;
    return XR_LIR_OK;
}

XR_FUNC XrLirStatus xr_lir_state_entry(const XrLirModule *m, XrLirFunction *out) {
    if (out) *out = (XrLirFunction){0};
    if (!out || !xr_lir_module_is_readable(m)) return XR_LIR_INVALID_ARGUMENT;
    if (m->state_entry) *out = (XrLirFunction){m, m->state_entry};
    return XR_LIR_OK;
}

XR_FUNC XrLirStatus xr_lir_state_reference_view(const XrLirModule *m, XrLirValue value,
    XrLirStateReference *out) {
    if (out) *out = (XrLirStateReference){0};
    if (!out || !xr_lir_module_is_readable(m) || value.module != m || !value.function ||
        value.function > m->function_count) return XR_LIR_INVALID_ARGUMENT;
    const XrLirFunctionData *f = &m->functions[value.function - 1];
    if (!value.id || value.id > f->value_count || f->values[value.id - 1].type != XR_LIR_STATE_REF)
        return XR_LIR_INVALID_ARGUMENT;
    *out = f->values[value.id - 1].state_reference;
    return XR_LIR_OK;
}
