/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_array.inc.c - Copying closed Array signatures and producer queries
 */
static const XrLirType lir_array_elements[XR_LIR_ARRAY_FAMILY_COUNT] = {
    XR_LIR_I64, XR_LIR_STRING, XR_LIR_U8, XR_LIR_U64
};
XR_FUNC uint32_t xr_lir_internal_array_family(XrLirType type) {
    for (uint32_t family = 0; family < XR_LIR_ARRAY_FAMILY_COUNT; ++family)
        if (lir_array_elements[family] == type) return family;
    return UINT32_MAX;
}
XR_FUNC bool xr_lir_internal_array_element(XrLirType type) {
    return xr_lir_internal_array_family(type) != UINT32_MAX;
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
        (shape->parameter_count && !shape->parameter_elements) ||
        entry->result == XR_LIR_ARRAY_CELL || entry->result == XR_LIR_ARRAY_REF)
        return false;
    if (entry->result == XR_LIR_ARRAY ? !xr_lir_internal_array_element(shape->result_element) :
        shape->result_element != XR_LIR_TYPE_INVALID) return false;
    bool array = entry->result == XR_LIR_ARRAY;
    if (entry->result == XR_LIR_STATE || entry->result == XR_LIR_STATE_REF ||
        entry->result == XR_LIR_STRING_CELL || entry->result == XR_LIR_I64_CELL) return false;
    for (uint32_t p = 0; p < entry->parameter_count; ++p) {
        array |= xr_lir_internal_array_type(entry->parameters[p]);
        if (entry->parameters[p] == XR_LIR_STATE || entry->parameters[p] == XR_LIR_STATE_REF ||
            (entry->parameters[p] == XR_LIR_ARRAY_REF && (p ||
                (shape->operation != XR_LIR_ARRAY_OP_SET && shape->operation != XR_LIR_ARRAY_OP_PUSH))) ||
            entry->parameters[p] == XR_LIR_STRING_CELL || entry->parameters[p] == XR_LIR_I64_CELL)
            return false;
        if (xr_lir_internal_array_type(entry->parameters[p]) ?
            !xr_lir_internal_array_element(shape->parameter_elements[p]) :
            shape->parameter_elements[p] != XR_LIR_TYPE_INVALID) return false;
    }
    if (!array) return false;
    if (shape->operation == XR_LIR_ARRAY_OP_NEW)
        return shape->variadic && entry->result == XR_LIR_ARRAY && entry->parameter_count == 1 &&
            entry->parameters[0] == shape->result_element;
    if (shape->operation == XR_LIR_ARRAY_OP_REPEAT)
        return !shape->variadic && entry->result == XR_LIR_ARRAY && entry->parameter_count == 2 &&
            entry->parameters[0] == XR_LIR_I64 && entry->parameters[1] == shape->result_element;
    if (shape->variadic || !entry->parameter_count) return false;
    XrLirType element = shape->parameter_elements[0];
    switch (shape->operation) {
    case XR_LIR_ARRAY_OP_LEN:
        return entry->parameter_count == 1 && entry->parameters[0] == XR_LIR_ARRAY && entry->result == XR_LIR_I64;
    case XR_LIR_ARRAY_OP_DISPLAY:
        return entry->parameter_count == 1 && entry->parameters[0] == XR_LIR_ARRAY && entry->result == XR_LIR_STRING;
    case XR_LIR_ARRAY_OP_GET:
        return entry->parameter_count == 2 && entry->parameters[0] == XR_LIR_ARRAY &&
            entry->parameters[1] == XR_LIR_I64 && entry->result == element;
    case XR_LIR_ARRAY_OP_SET:
        return entry->parameter_count == 3 &&
            (entry->parameters[0] == XR_LIR_ARRAY_CELL || entry->parameters[0] == XR_LIR_ARRAY_REF) &&
            entry->parameters[1] == XR_LIR_I64 && entry->parameters[2] == element && entry->result == XR_LIR_UNIT;
    case XR_LIR_ARRAY_OP_PUSH:
        return entry->parameter_count == 2 &&
            (entry->parameters[0] == XR_LIR_ARRAY_CELL || entry->parameters[0] == XR_LIR_ARRAY_REF) &&
            entry->parameters[1] == element && entry->result == XR_LIR_UNIT;
    default: return false;
    }
}

XR_FUNC XrLirStatus xr_lir_set_array_intrinsic(XrLirModule *m,
    const XrLirArrayIntrinsic *input) {
    XrLirStatus status = lir_writable(m);
    if (status != XR_LIR_OK) return status;
    uint32_t index = input ? lir_array_intrinsic_index(m, input->id) : UINT32_MAX;
    if (index == UINT32_MAX || !xr_lir_internal_array_intrinsic_valid(&m->intrinsics[index], input) ||
        (m->array_intrinsics && m->array_intrinsics[index].id))
        return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
    XrLirType element = m->intrinsics[index].result == XR_LIR_ARRAY ?
        input->result_element : input->parameter_elements[0];
    uint32_t family = xr_lir_internal_array_family(element);
    uint32_t operation = (uint32_t)input->operation - 1;
    bool reference = m->intrinsics[index].parameter_count && m->intrinsics[index].parameters[0] == XR_LIR_ARRAY_REF;
    uint32_t *operation_id = reference ? &m->array_ref_operations[family][(uint32_t)input->operation - XR_LIR_ARRAY_OP_SET] :
        &m->array_operations[family][operation];
    if (*operation_id)
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
    *operation_id = input->id;
    return XR_LIR_OK;
}

XR_FUNC XrLirStatus xr_lir_array_operation(const XrLirModule *m, XrLirType element,
    XrLirArrayOperation operation, uint32_t *id) {
    if (id) *id = 0;
    if (!id || !m || m->failure != XR_LIR_OK || !xr_lir_internal_array_element(element) ||
        operation < XR_LIR_ARRAY_OP_NEW || operation > XR_LIR_ARRAY_OP_DISPLAY)
        return XR_LIR_INVALID_ARGUMENT;
    *id = m->array_operations[xr_lir_internal_array_family(element)][(uint32_t)operation - 1];
    return *id ? XR_LIR_OK : XR_LIR_UNSUPPORTED;
}

XR_FUNC XrLirStatus xr_lir_array_mutator(const XrLirModule *m, XrLirType element,
    XrLirType receiver, XrLirArrayOperation operation, uint32_t *id) {
    if (id) *id = 0;
    if (!id || !m || m->failure != XR_LIR_OK || !xr_lir_internal_array_element(element) ||
        (receiver != XR_LIR_ARRAY_CELL && receiver != XR_LIR_ARRAY_REF) ||
        (operation != XR_LIR_ARRAY_OP_SET && operation != XR_LIR_ARRAY_OP_PUSH))
        return XR_LIR_INVALID_ARGUMENT;
    if (receiver == XR_LIR_ARRAY_CELL) return xr_lir_array_operation(m, element, operation, id);
    *id = m->array_ref_operations[xr_lir_internal_array_family(element)][(uint32_t)operation - XR_LIR_ARRAY_OP_SET];
    return *id ? XR_LIR_OK : XR_LIR_UNSUPPORTED;
}

static XrLirStatus lir_array_catalog(XrLirModule *m, const XrLirRegistry *extra) {
    enum { operations = XR_LIR_ARRAY_OP_REPEAT, base_catalog = XR_LIR_ARRAY_FAMILY_COUNT * operations,
        reference_end = base_catalog + XR_LIR_ARRAY_FAMILY_COUNT * 2,
        catalog = XR_LIR_ARRAY_CATALOG_COUNT };
    uint32_t count = extra ? extra->count : 0;
    if (!m || m->intrinsic_count || m->function_count || (count && !extra->entries) || count > UINT32_MAX - catalog - 1 ||
        (size_t)count + catalog > SIZE_MAX / sizeof(XrLirIntrinsic))
        return XR_LIR_INVALID_ARGUMENT;
    uint32_t total = count + catalog;
    XrLirIntrinsic *entries = xr_malloc((size_t)total * sizeof(*entries));
    uint8_t *used = xr_malloc((size_t)total + 1);
    if (!entries || !used) { xr_free(entries); xr_free(used); return XR_LIR_OUT_OF_MEMORY; }
    if (count) memcpy(entries, extra->entries, (size_t)count * sizeof(*entries));
    memset(used, 0, (size_t)total + 1);
    for (uint32_t i = 0; i < count; ++i)
        if (entries[i].id <= total) used[entries[i].id] = 1;
    XrLirType parameters[catalog][3] = {{0}}, shapes[catalog][3] = {{0}};
    XrLirArrayIntrinsic descriptors[catalog] = {{0}};
    uint32_t next = 1;
    for (uint32_t i = 0; i < catalog; ++i) {
        while (used[next]) ++next;
        used[next] = 1;
        bool reference = i >= base_catalog && i < reference_end;
        bool display = i >= reference_end;
        uint32_t family = display ? i - reference_end : reference ? (i - base_catalog) / 2 : i / operations;
        XrLirType element = lir_array_elements[family];
        XrLirArrayOperation operation = display ? XR_LIR_ARRAY_OP_DISPLAY : reference ?
            (XrLirArrayOperation)(XR_LIR_ARRAY_OP_SET + (i - base_catalog) % 2) :
            (XrLirArrayOperation)(i % operations + 1);
        uint32_t arity = 1;
        XrLirType result = XR_LIR_UNIT;
        parameters[i][0] = XR_LIR_ARRAY;
        shapes[i][0] = element;
        switch (operation) {
        case XR_LIR_ARRAY_OP_NEW:
            parameters[i][0] = element; shapes[i][0] = XR_LIR_TYPE_INVALID;
            result = XR_LIR_ARRAY; break;
        case XR_LIR_ARRAY_OP_LEN: result = XR_LIR_I64; break;
        case XR_LIR_ARRAY_OP_DISPLAY: result = XR_LIR_STRING; break;
        case XR_LIR_ARRAY_OP_GET:
            arity = 2; parameters[i][1] = XR_LIR_I64; result = element; break;
        case XR_LIR_ARRAY_OP_SET:
            arity = 3; parameters[i][0] = reference ? XR_LIR_ARRAY_REF : XR_LIR_ARRAY_CELL;
            parameters[i][1] = XR_LIR_I64; parameters[i][2] = element; break;
        case XR_LIR_ARRAY_OP_PUSH:
            arity = 2; parameters[i][0] = reference ? XR_LIR_ARRAY_REF : XR_LIR_ARRAY_CELL;
            parameters[i][1] = element; break;
        case XR_LIR_ARRAY_OP_REPEAT:
            arity = 2; parameters[i][0] = XR_LIR_I64; parameters[i][1] = element;
            shapes[i][0] = XR_LIR_TYPE_INVALID; result = XR_LIR_ARRAY; break;
        default: break;
        }
        entries[count + i] = (XrLirIntrinsic){next, parameters[i], arity, result, true, 0};
        descriptors[i] = (XrLirArrayIntrinsic){next, shapes[i], arity,
            result == XR_LIR_ARRAY ? element : XR_LIR_TYPE_INVALID,
            operation == XR_LIR_ARRAY_OP_NEW, operation};
    }
    XrLirRegistry registry = {entries, total};
    XrLirStatus status = xr_lir_set_registry(m, &registry);
    for (uint32_t i = 0; status == XR_LIR_OK && i < catalog; ++i)
        status = xr_lir_set_array_intrinsic(m, &descriptors[i]);
    xr_free(entries); xr_free(used);
    return status;
}

XR_FUNC XrLirStatus xr_lir_array_catalog_install(XrLirModule *m) {
    XrLirStatus status = lir_writable(m);
    if (status != XR_LIR_OK) return status;
    status = lir_array_catalog(m, NULL);
    return status == XR_LIR_OK ? status : lir_fail(m, status);
}

XR_FUNC XrLirStatus xr_lir_array_module_create(const XrLirRegistry *extra, XrLirModule **output) {
    if (!output || *output) return XR_LIR_INVALID_ARGUMENT;
    XrLirModule *m = NULL;
    XrLirStatus status = xr_lir_module_create(NULL, &m);
    if (status == XR_LIR_OK) status = lir_array_catalog(m, extra);
    if (status != XR_LIR_OK) { xr_lir_module_destroy(m); return status; }
    *output = m;
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
    const XrLirValueData *input = v->operand_count && v->operands[0] && v->operands[0] <= f->value_count ?
        &f->values[v->operands[0] - 1] : NULL;
    if (input && (v->op == XR_LIR_STATE_READ ||
        (v->op == XR_LIR_ARRAY_BORROW && input->type == XR_LIR_STATE_REF)))
        return input->state_reference.array_element;
    if (v->op == XR_LIR_CALL && v->callee && v->callee <= m->function_count)
        return m->functions[v->callee - 1].array_result;
    if (v->op == XR_LIR_CALL_INTRINSIC) {
        uint32_t index = lir_array_intrinsic_index(m, v->intrinsic);
        if (index != UINT32_MAX && (m->intrinsics[index].view_operation == XR_LIR_VIEW_OP_CELL_READ ||
            m->intrinsics[index].view_operation == XR_LIR_VIEW_OP_COPY)) return XR_LIR_I64;
        return index != UINT32_MAX && m->array_intrinsics ?
            m->array_intrinsics[index].result_element : XR_LIR_TYPE_INVALID;
    }
    if (input && (v->op == XR_LIR_CELL_NEW || v->op == XR_LIR_CELL_READ || v->op == XR_LIR_ARRAY_RETAIN ||
        v->op == XR_LIR_ARRAY_BORROW || v->op == XR_LIR_ARRAY_REF_READ))
        return input->array_element;
    return XR_LIR_TYPE_INVALID;
}

XR_FUNC const XrLirArrayIntrinsic *xr_lir_internal_array_intrinsic(const XrLirModule *m,
    uint32_t id) {
    uint32_t index = lir_array_intrinsic_index(m, id);
    return index != UINT32_MAX && m->array_intrinsics && m->array_intrinsics[index].id ?
        &m->array_intrinsics[index] : NULL;
}
