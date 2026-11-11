/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_array.inc.c - Copying closed Array signatures and producer queries
 */
XR_FUNC bool xr_lir_internal_array_element(XrLirType type) {
    return type == XR_LIR_I64 || type == XR_LIR_STRING;
}

static uint32_t lir_array_intrinsic_index(const XrLirModule *m, uint32_t id) {
    for (uint32_t i = 0; i < m->intrinsic_count; ++i)
        if (m->intrinsics[i].id == id) return i;
    return UINT32_MAX;
}

XR_FUNC XrLirStatus xr_lir_set_array_shape(XrLirModule *m, XrLirValue value,
    const XrLirArrayShape *shape) {
    XrLirStatus status = lir_writable(m);
    if (status != XR_LIR_OK) return status;
    if (!shape || !xr_lir_internal_array_element(shape->element) || value.module != m ||
        !value.function || value.function > m->function_count)
        return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
    XrLirFunctionData *f = &m->functions[value.function - 1];
    if (!value.id || value.id > f->value_count) return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
    XrLirValueData *v = &f->values[value.id - 1];
    if (!v->parameter || !xr_lir_internal_array_type(v->type) || v->array_element)
        return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
    v->array_element = shape->element;
    return XR_LIR_OK;
}

XR_FUNC XrLirStatus xr_lir_set_array_result(XrLirModule *m, XrLirFunction function,
    const XrLirArrayShape *shape) {
    XrLirStatus status = lir_writable(m);
    if (status != XR_LIR_OK) return status;
    XrLirFunctionData *f = lir_function(m, function);
    if (!f || !shape || !xr_lir_internal_array_element(shape->element) ||
        f->result != XR_LIR_ARRAY || f->array_result)
        return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
    f->array_result = shape->element;
    return XR_LIR_OK;
}

XR_FUNC bool xr_lir_internal_array_intrinsic_valid(const XrLirIntrinsic *entry,
    const XrLirArrayIntrinsic *shape) {
    if (shape->parameter_count != entry->parameter_count ||
        (shape->parameter_count && !shape->parameter_elements) || entry->result == XR_LIR_ARRAY_CELL)
        return false;
    if (entry->result == XR_LIR_ARRAY ? !xr_lir_internal_array_element(shape->result_element) :
        shape->result_element != XR_LIR_TYPE_INVALID) return false;
    bool array = entry->result == XR_LIR_ARRAY;
    if (entry->result == XR_LIR_STATE || entry->result == XR_LIR_STATE_REF ||
        entry->result == XR_LIR_STRING_CELL || entry->result == XR_LIR_I64_CELL) return false;
    for (uint32_t p = 0; p < entry->parameter_count; ++p) {
        array |= xr_lir_internal_array_type(entry->parameters[p]);
        if (entry->parameters[p] == XR_LIR_STATE || entry->parameters[p] == XR_LIR_STATE_REF ||
            entry->parameters[p] == XR_LIR_STRING_CELL || entry->parameters[p] == XR_LIR_I64_CELL)
            return false;
        if (xr_lir_internal_array_type(entry->parameters[p]) ?
            !xr_lir_internal_array_element(shape->parameter_elements[p]) :
            shape->parameter_elements[p] != XR_LIR_TYPE_INVALID) return false;
    }
    return array && (!shape->variadic || (entry->result == XR_LIR_ARRAY && entry->parameter_count == 1 &&
        entry->parameters[0] == shape->result_element));
}

XR_FUNC XrLirStatus xr_lir_set_array_intrinsic(XrLirModule *m,
    const XrLirArrayIntrinsic *input) {
    XrLirStatus status = lir_writable(m);
    if (status != XR_LIR_OK) return status;
    uint32_t index = input ? lir_array_intrinsic_index(m, input->id) : UINT32_MAX;
    if (index == UINT32_MAX || !xr_lir_internal_array_intrinsic_valid(&m->intrinsics[index], input) ||
        (m->array_intrinsics && m->array_intrinsics[index].id))
        return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
    XrLirType *parameters = lir_copy(m, input->parameter_elements,
        input->parameter_count, sizeof(*parameters));
    if (input->parameter_count && !parameters) return m->failure;
    if (!m->array_intrinsics) {
        m->array_intrinsics = lir_allocate(m, m->intrinsic_count, sizeof(*m->array_intrinsics));
        if (!m->array_intrinsics) { xr_free(parameters); return m->failure; }
        memset(m->array_intrinsics, 0, (size_t)m->intrinsic_count * sizeof(*m->array_intrinsics));
    }
    m->array_intrinsics[index] = *input;
    m->array_intrinsics[index].parameter_elements = parameters;
    return XR_LIR_OK;
}

XR_FUNC XrLirStatus xr_lir_array_shape_view(const XrLirModule *m, XrLirValue value,
    XrLirArrayShape *output) {
    if (output) *output = (XrLirArrayShape){0};
    if (!output || !xr_lir_module_is_readable(m) || value.module != m || !value.function ||
        value.function > m->function_count) return XR_LIR_INVALID_ARGUMENT;
    const XrLirFunctionData *f = &m->functions[value.function - 1];
    if (!value.id || value.id > f->value_count || !xr_lir_internal_array_type(f->values[value.id - 1].type))
        return XR_LIR_INVALID_ARGUMENT;
    output->element = f->values[value.id - 1].array_element;
    return XR_LIR_OK;
}

XR_FUNC XrLirStatus xr_lir_array_result_view(const XrLirModule *m, XrLirFunction function,
    XrLirArrayShape *output) {
    if (output) *output = (XrLirArrayShape){0};
    if (!output || !xr_lir_module_is_readable(m) || function.module != m || !function.id ||
        function.id > m->function_count) return XR_LIR_INVALID_ARGUMENT;
    output->element = m->functions[function.id - 1].array_result;
    return XR_LIR_OK;
}

XR_FUNC XrLirStatus xr_lir_array_intrinsic_view(const XrLirModule *m, uint32_t id,
    XrLirArrayIntrinsic *output) {
    if (output) *output = (XrLirArrayIntrinsic){0};
    if (!output || !xr_lir_module_is_readable(m)) return XR_LIR_INVALID_ARGUMENT;
    uint32_t index = lir_array_intrinsic_index(m, id);
    if (index == UINT32_MAX) return XR_LIR_INVALID_ARGUMENT;
    if (m->array_intrinsics) *output = m->array_intrinsics[index];
    return XR_LIR_OK;
}

XR_FUNC XrLirType xr_lir_internal_array_result(const XrLirModule *m,
    const XrLirFunctionData *f, const XrLirInstructionView *v) {
    if (!xr_lir_internal_array_type(v->type)) return XR_LIR_TYPE_INVALID;
    if (v->op == XR_LIR_CALL && v->callee && v->callee <= m->function_count)
        return m->functions[v->callee - 1].array_result;
    if (v->op == XR_LIR_CALL_INTRINSIC) {
        uint32_t index = lir_array_intrinsic_index(m, v->intrinsic);
        return index != UINT32_MAX && m->array_intrinsics ?
            m->array_intrinsics[index].result_element : XR_LIR_TYPE_INVALID;
    }
    if ((v->op == XR_LIR_CELL_NEW || v->op == XR_LIR_CELL_READ || v->op == XR_LIR_ARRAY_RETAIN) &&
        v->operand_count && v->operands[0] && v->operands[0] <= f->value_count)
        return f->values[v->operands[0] - 1].array_element;
    return XR_LIR_TYPE_INVALID;
}

XR_FUNC const XrLirArrayIntrinsic *xr_lir_internal_array_intrinsic(const XrLirModule *m,
    uint32_t id) {
    uint32_t index = lir_array_intrinsic_index(m, id);
    return index != UINT32_MAX && m->array_intrinsics && m->array_intrinsics[index].id ?
        &m->array_intrinsics[index] : NULL;
}
