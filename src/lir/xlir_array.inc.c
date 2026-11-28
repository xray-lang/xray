/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_array.inc.c - Copying closed Array signatures and producer queries
 */
static const XrLirType lir_array_elements[XR_LIR_ARRAY_FAMILY_COUNT] = {
    XR_LIR_I64, XR_LIR_STRING, XR_LIR_U8, XR_LIR_U64, XR_LIR_F64
};
XR_FUNC XrLirType xr_lir_internal_array_element_at(uint32_t family) {
    return family < XR_LIR_ARRAY_FAMILY_COUNT ? lir_array_elements[family] : XR_LIR_TYPE_INVALID;
}
XR_FUNC uint32_t xr_lir_internal_array_family(XrLirType type) {
    for (uint32_t family = 0; family < XR_LIR_ARRAY_FAMILY_COUNT; ++family)
        if (lir_array_elements[family] == type) return family;
    return UINT32_MAX;
}
XR_FUNC uint32_t xr_lir_inline_array_ordinal(const XrLirModule *m, XrLirType type) {
    return m && type >= XR_LIR_TYPE_COUNT && type - XR_LIR_TYPE_COUNT < m->type_count
        ? m->types[type - XR_LIR_TYPE_COUNT].inline_array_ordinal : 0;
}
XR_FUNC bool xr_lir_class_array_push_element(const XrLirModule *m, XrLirType element) {
    return xr_lir_inline_array_ordinal(m, element) && xr_lir_class_payload(m, element, NULL) &&
        !xr_lir_class_storage(m, element);
}
static bool lir_inline_array_element_supported(const XrLirModule *m, XrLirType type) {
    const XrLirTypeData *data = &m->types[type - XR_LIR_TYPE_COUNT];
    XrLirTypeKind kind = data->shape.kind;
    return (kind == XR_LIR_TYPE_RECORD && !data->class_payload_only) || kind == XR_LIR_TYPE_TUPLE ||
        (kind == XR_LIR_TYPE_CLASS && !xr_lir_class_storage(m, type));
}
XR_FUNC bool xr_lir_internal_array_element(const XrLirModule *m, XrLirType type) {
    return xr_lir_internal_array_family(type) != UINT32_MAX ||
        (xr_lir_inline_array_ordinal(m, type) && lir_inline_array_element_supported(m, type));
}
XR_FUNC uint32_t xr_lir_array_layout_family(const XrLirModule *m, XrLirType type) {
    uint32_t fixed = xr_lir_internal_array_family(type);
    uint32_t ordinal = xr_lir_inline_array_ordinal(m, type);
    return ordinal ? XR_LIR_ARRAY_FAMILY_COUNT + ordinal - 1 : fixed;
}
XR_FUNC XrLirStatus xr_lir_require_inline_array_element(XrLirModule *m, XrLirType type) {
    XrLirStatus status = lir_writable(m);
    if (status != XR_LIR_OK) return status;
    if (type < XR_LIR_TYPE_COUNT || type - XR_LIR_TYPE_COUNT >= m->type_count)
        return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
    XrLirTypeData *data = &m->types[type - XR_LIR_TYPE_COUNT];
    if (!lir_inline_array_element_supported(m, type))
        return lir_fail(m, XR_LIR_UNSUPPORTED);
    if (!data->inline_array_ordinal) {
        if (m->intrinsic_count) return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
        data->inline_array_ordinal = 1;
        ++m->inline_array_count;
    }
    return XR_LIR_OK;
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
    if (!shape || !xr_lir_internal_array_element(m, shape->element) || value.module != m ||
        !value.function || value.function > m->function_count)
        return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
    XrLirFunctionData *f = &m->functions[value.function - 1];
    if (!value.id || value.id > f->value_count) return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
    XrLirValueData *v = &f->values[value.id - 1];
    if (!v->parameter || !xr_lir_internal_array_type(v->type) || v->array_element ||
        (v->type != XR_LIR_ARRAY && xr_lir_inline_array_ordinal(m, shape->element)))
        return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
    v->array_element = shape->element;
    return XR_LIR_OK;
}

XR_FUNC XrLirStatus xr_lir_set_array_result(XrLirModule *m, XrLirFunction function,
    const XrLirArrayShape *shape) {
    XrLirStatus status = lir_writable(m);
    if (status != XR_LIR_OK) return status;
    XrLirFunctionData *f = lir_function(m, function);
    if (!f || !shape || !xr_lir_internal_array_element(m, shape->element) ||
        f->result != XR_LIR_ARRAY || f->array_result)
        return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
    f->array_result = shape->element;
    return XR_LIR_OK;
}

XR_FUNC bool xr_lir_internal_array_intrinsic_valid(const XrLirModule *m, const XrLirIntrinsic *entry,
    const XrLirArrayIntrinsic *shape) {
    if (shape->parameter_count != entry->parameter_count ||
        (shape->parameter_count && !shape->parameter_elements) ||
        entry->result == XR_LIR_ARRAY_CELL || entry->result == XR_LIR_ARRAY_REF)
        return false;
    if (entry->result == XR_LIR_ARRAY ? !xr_lir_internal_array_element(m, shape->result_element) :
        shape->result_element != XR_LIR_TYPE_INVALID) return false;
    bool array = entry->result == XR_LIR_ARRAY;
    if (entry->result == XR_LIR_STATE || entry->result == XR_LIR_STATE_REF ||
        entry->result == XR_LIR_STRING_CELL || entry->result == XR_LIR_I64_CELL) return false;
    for (uint32_t p = 0; p < entry->parameter_count; ++p) {
        array |= xr_lir_internal_array_type(entry->parameters[p]);
        if (entry->parameters[p] == XR_LIR_STATE || entry->parameters[p] == XR_LIR_STATE_REF ||
            (entry->parameters[p] == XR_LIR_ARRAY_REF && (p ||
                !xr_lir_array_binding_update(shape->operation))) ||
            entry->parameters[p] == XR_LIR_STRING_CELL || entry->parameters[p] == XR_LIR_I64_CELL)
            return false;
        if (xr_lir_internal_array_type(entry->parameters[p]) ?
            !xr_lir_internal_array_element(m, shape->parameter_elements[p]) :
            shape->parameter_elements[p] != XR_LIR_TYPE_INVALID) return false;
    }
    if (!array) return false;
    XrLirType element = entry->result == XR_LIR_ARRAY ? shape->result_element : shape->parameter_elements[0];
    bool class_push = shape->operation == XR_LIR_ARRAY_OP_PUSH && entry->parameter_count == 2 &&
        entry->parameters[0] == XR_LIR_ARRAY_CELL && xr_lir_class_array_push_element(m, element);
    if (xr_lir_inline_array_ordinal(m, element) && !class_push &&
        (shape->operation < XR_LIR_ARRAY_OP_NEW || shape->operation > XR_LIR_ARRAY_OP_GET))
        return false;
    if (shape->operation == XR_LIR_ARRAY_OP_NEW)
        return shape->variadic && entry->result == XR_LIR_ARRAY && entry->parameter_count == 1 &&
            entry->parameters[0] == shape->result_element;
    if (shape->operation == XR_LIR_ARRAY_OP_REPEAT)
        return !shape->variadic && entry->result == XR_LIR_ARRAY && entry->parameter_count == 2 &&
            entry->parameters[0] == XR_LIR_I64 && entry->parameters[1] == shape->result_element;
    if (shape->operation == XR_LIR_ARRAY_OP_WITH_CAPACITY)
        return !shape->variadic && entry->result == XR_LIR_ARRAY && entry->parameter_count == 1 &&
            entry->parameters[0] == XR_LIR_I64;
    if (shape->variadic || !entry->parameter_count) return false;
    switch (shape->operation) {
    case XR_LIR_ARRAY_OP_LEN: case XR_LIR_ARRAY_OP_CAPACITY:
        return entry->parameter_count == 1 && entry->parameters[0] == XR_LIR_ARRAY && entry->result == XR_LIR_I64;
    case XR_LIR_ARRAY_OP_RESERVE:
        return entry->parameter_count == 2 && entry->parameters[0] == XR_LIR_ARRAY &&
            entry->parameters[1] == XR_LIR_I64 && entry->result == XR_LIR_ARRAY &&
            shape->parameter_elements[0] == shape->result_element;
    case XR_LIR_ARRAY_OP_COPY_STRING_BYTES:
        return entry->parameter_count == 1 && entry->parameters[0] == XR_LIR_STRING &&
            entry->result == XR_LIR_ARRAY && shape->result_element == XR_LIR_U8;
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
    case XR_LIR_ARRAY_OP_TIMING_EQUAL:
        return entry->parameter_count == 2 && entry->parameters[0] == XR_LIR_ARRAY &&
            entry->parameters[1] == XR_LIR_ARRAY && entry->result == XR_LIR_I1 &&
            element == XR_LIR_U8 && shape->parameter_elements[1] == XR_LIR_U8;
    case XR_LIR_ARRAY_OP_RANDOM_BYTES:
        return entry->parameter_count == 1 && entry->parameters[0] == XR_LIR_ARRAY_REF &&
            element == XR_LIR_U8 && entry->result == XR_LIR_UNIT;
    default: return false;
    }
}

XR_FUNC XrLirStatus xr_lir_set_array_intrinsic(XrLirModule *m,
    const XrLirArrayIntrinsic *input) {
    XrLirStatus status = lir_writable(m);
    if (status != XR_LIR_OK) return status;
    uint32_t index = input ? lir_array_intrinsic_index(m, input->id) : UINT32_MAX;
    if (index == UINT32_MAX || !xr_lir_internal_array_intrinsic_valid(m, &m->intrinsics[index], input) ||
        (m->array_intrinsics && m->array_intrinsics[index].id))
        return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
    XrLirType element = m->intrinsics[index].result == XR_LIR_ARRAY ?
        input->result_element : input->parameter_elements[0];
    uint32_t family = xr_lir_internal_array_family(element);
    uint32_t operation = (uint32_t)input->operation - 1;
    bool reference = m->intrinsics[index].parameter_count && m->intrinsics[index].parameters[0] == XR_LIR_ARRAY_REF;
    bool inline_array = family == UINT32_MAX;
    uint32_t *operation_id = input->operation == XR_LIR_ARRAY_OP_RANDOM_BYTES ? &m->array_ref_random_bytes :
        input->operation == XR_LIR_ARRAY_OP_TIMING_EQUAL ? &m->array_u8_timing_equal :
        inline_array ? &m->types[element - XR_LIR_TYPE_COUNT].inline_array_operations[operation] :
        reference ? &m->array_ref_operations[family][(uint32_t)input->operation - XR_LIR_ARRAY_OP_SET] :
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
    *operation_id = inline_array ? index + 1 : input->id;
    return XR_LIR_OK;
}

XR_FUNC XrLirStatus xr_lir_array_operation(const XrLirModule *m, XrLirType element,
    XrLirArrayOperation operation, uint32_t *id) {
    if (id) *id = 0;
    if (!id || !m || m->failure != XR_LIR_OK || !xr_lir_internal_array_element(m, element) ||
        operation < XR_LIR_ARRAY_OP_NEW || (operation > XR_LIR_ARRAY_OP_WITH_CAPACITY &&
            operation != XR_LIR_ARRAY_OP_TIMING_EQUAL))
        return XR_LIR_INVALID_ARGUMENT;
    if (operation == XR_LIR_ARRAY_OP_TIMING_EQUAL) {
        if (element != XR_LIR_U8) return XR_LIR_UNSUPPORTED;
        *id = m->array_u8_timing_equal;
        return *id ? XR_LIR_OK : XR_LIR_UNSUPPORTED;
    }
    uint32_t family = xr_lir_internal_array_family(element);
    if (family == UINT32_MAX) {
        if (operation > XR_LIR_ARRAY_OP_GET &&
            (operation != XR_LIR_ARRAY_OP_PUSH || !xr_lir_class_array_push_element(m, element)))
            return XR_LIR_UNSUPPORTED;
        uint32_t index = m->types[element - XR_LIR_TYPE_COUNT].inline_array_operations[operation - 1];
        if (index) *id = m->intrinsics[index - 1].id;
    } else *id = m->array_operations[family][(uint32_t)operation - 1];
    return *id ? XR_LIR_OK : XR_LIR_UNSUPPORTED;
}

XR_FUNC XrLirStatus xr_lir_array_mutator(const XrLirModule *m, XrLirType element,
    XrLirType receiver, XrLirArrayOperation operation, uint32_t *id) {
    if (id) *id = 0;
    if (!id || !m || m->failure != XR_LIR_OK || !xr_lir_internal_array_element(m, element) ||
        (receiver != XR_LIR_ARRAY_CELL && receiver != XR_LIR_ARRAY_REF) ||
        !xr_lir_array_binding_update(operation)) return XR_LIR_INVALID_ARGUMENT;
    if (operation == XR_LIR_ARRAY_OP_RANDOM_BYTES) {
        if (receiver != XR_LIR_ARRAY_REF || element != XR_LIR_U8) return XR_LIR_UNSUPPORTED;
        *id = m->array_ref_random_bytes;
        return *id ? XR_LIR_OK : XR_LIR_UNSUPPORTED;
    }
    if (receiver == XR_LIR_ARRAY_CELL) return xr_lir_array_operation(m, element, operation, id);
    if (xr_lir_inline_array_ordinal(m, element)) return XR_LIR_UNSUPPORTED;
    *id = m->array_ref_operations[xr_lir_internal_array_family(element)][(uint32_t)operation - XR_LIR_ARRAY_OP_SET];
    return *id ? XR_LIR_OK : XR_LIR_UNSUPPORTED;
}

typedef struct LirArrayCatalogRow {
    XrLirType parameters[3], shapes[3];
    XrLirArrayIntrinsic description;
} LirArrayCatalogRow;

static void lir_array_catalog_signature(LirArrayCatalogRow *row, XrLirIntrinsic *entry, uint32_t id,
    XrLirType element, XrLirArrayOperation operation, bool reference) {
    *row = (LirArrayCatalogRow){0};
    XrLirType *parameters = row->parameters, *shapes = row->shapes;
    uint32_t arity = 1;
    XrLirType result = XR_LIR_UNIT;
    parameters[0] = XR_LIR_ARRAY;
    shapes[0] = element;
    switch (operation) {
    case XR_LIR_ARRAY_OP_NEW:
        parameters[0] = element; shapes[0] = XR_LIR_TYPE_INVALID;
        result = XR_LIR_ARRAY; break;
    case XR_LIR_ARRAY_OP_LEN: case XR_LIR_ARRAY_OP_CAPACITY: result = XR_LIR_I64; break;
    case XR_LIR_ARRAY_OP_DISPLAY: result = XR_LIR_STRING; break;
    case XR_LIR_ARRAY_OP_RESERVE:
        arity = 2; parameters[1] = XR_LIR_I64; result = XR_LIR_ARRAY; break;
    case XR_LIR_ARRAY_OP_COPY_STRING_BYTES:
        parameters[0] = XR_LIR_STRING; shapes[0] = XR_LIR_TYPE_INVALID;
        result = XR_LIR_ARRAY; break;
    case XR_LIR_ARRAY_OP_GET:
        arity = 2; parameters[1] = XR_LIR_I64; result = element; break;
    case XR_LIR_ARRAY_OP_SET:
        arity = 3; parameters[0] = reference ? XR_LIR_ARRAY_REF : XR_LIR_ARRAY_CELL;
        parameters[1] = XR_LIR_I64; parameters[2] = element; break;
    case XR_LIR_ARRAY_OP_PUSH:
        arity = 2; parameters[0] = reference ? XR_LIR_ARRAY_REF : XR_LIR_ARRAY_CELL;
        parameters[1] = element; break;
    case XR_LIR_ARRAY_OP_TIMING_EQUAL:
        arity = 2; parameters[1] = XR_LIR_ARRAY; shapes[1] = element; result = XR_LIR_I1; break;
    case XR_LIR_ARRAY_OP_RANDOM_BYTES:
        parameters[0] = XR_LIR_ARRAY_REF; break;
    case XR_LIR_ARRAY_OP_REPEAT:
        arity = 2; parameters[0] = XR_LIR_I64; parameters[1] = element;
        shapes[0] = XR_LIR_TYPE_INVALID; result = XR_LIR_ARRAY; break;
    case XR_LIR_ARRAY_OP_WITH_CAPACITY:
        parameters[0] = XR_LIR_I64; shapes[0] = XR_LIR_TYPE_INVALID;
        result = XR_LIR_ARRAY; break;
    default: XR_CHECK(false, "Array catalog admits only declared operations"); break;
    }
    *entry = (XrLirIntrinsic){id, parameters, arity, result, true, 0};
    row->description = (XrLirArrayIntrinsic){id, shapes, arity,
        result == XR_LIR_ARRAY ? element : XR_LIR_TYPE_INVALID, operation == XR_LIR_ARRAY_OP_NEW, operation};
}

static XrLirStatus lir_array_catalog(XrLirModule *m, const XrLirRegistry *extra) {
    enum { operations = XR_LIR_ARRAY_OP_REPEAT, base_catalog = XR_LIR_ARRAY_FAMILY_COUNT * operations,
        reference_end = base_catalog + XR_LIR_ARRAY_FAMILY_COUNT * 2,
        display_end = reference_end + XR_LIR_ARRAY_FAMILY_COUNT,
        capacity_end = display_end + XR_LIR_ARRAY_FAMILY_COUNT * 2 };
    uint32_t catalog = XR_LIR_ARRAY_CATALOG_COUNT + (m && m->needs_array_timing_equal);
    uint32_t count = extra ? extra->count : 0;
    if (!m || m->intrinsic_count || m->function_count || (count && !extra->entries) || count > UINT32_MAX - catalog - 1)
        return XR_LIR_INVALID_ARGUMENT;
    uint64_t inline_rows = (uint64_t)m->inline_array_count * XR_LIR_ARRAY_OP_GET;
    for (uint32_t t = 0; t < m->type_count; ++t)
        if (xr_lir_class_array_push_element(m, XR_LIR_TYPE_COUNT + t)) ++inline_rows;
    if (inline_rows > UINT32_MAX - catalog - 1 - count) return XR_LIR_INVALID_ARGUMENT;
    uint32_t rows = catalog + (uint32_t)inline_rows;
    uint32_t total = count + rows;
    size_t entry_bytes = (size_t)total * sizeof(XrLirIntrinsic);
    size_t row_bytes = (size_t)rows * sizeof(LirArrayCatalogRow);
    if (entry_bytes / sizeof(XrLirIntrinsic) != total || row_bytes / sizeof(LirArrayCatalogRow) != rows)
        return XR_LIR_INVALID_ARGUMENT;
    XrLirIntrinsic *entries = xr_malloc(entry_bytes);
    uint8_t *used = xr_malloc((size_t)total + 1);
    LirArrayCatalogRow fixed[XR_LIR_ARRAY_CATALOG_COUNT + 1];
    LirArrayCatalogRow *parts = rows == catalog ? fixed : xr_malloc(row_bytes);
    if (!entries || !used || !parts) {
        xr_free(entries); xr_free(used);
        if (parts != fixed) xr_free(parts);
        return XR_LIR_OUT_OF_MEMORY;
    }
    if (count) memcpy(entries, extra->entries, (size_t)count * sizeof(*entries));
    memset(used, 0, (size_t)total + 1);
    for (uint32_t i = 0; i < count; ++i)
        if (entries[i].id <= total) used[entries[i].id] = 1;
    uint32_t next = 1;
    for (uint32_t i = 0; i < catalog; ++i) {
        while (used[next]) ++next;
        used[next] = 1;
        if (i >= XR_LIR_ARRAY_CATALOG_COUNT - 2) {
            static const XrLirArrayOperation bytes[] = {XR_LIR_ARRAY_OP_COPY_STRING_BYTES,
                XR_LIR_ARRAY_OP_RANDOM_BYTES, XR_LIR_ARRAY_OP_TIMING_EQUAL};
            XrLirArrayOperation operation = bytes[i - (XR_LIR_ARRAY_CATALOG_COUNT - 2)];
            lir_array_catalog_signature(&parts[i], &entries[count + i], next, XR_LIR_U8, operation, false);
            continue;
        }
        bool reference = i >= base_catalog && i < reference_end;
        uint32_t family;
        XrLirArrayOperation operation;
        if (i < base_catalog) {
            family = i / operations;
            operation = (XrLirArrayOperation)(i % operations + 1);
        } else if (reference) {
            family = (i - base_catalog) / 2;
            operation = (XrLirArrayOperation)(XR_LIR_ARRAY_OP_SET + (i - base_catalog) % 2);
        } else if (i < display_end) {
            family = i - reference_end;
            operation = XR_LIR_ARRAY_OP_DISPLAY;
        } else if (i < capacity_end) {
            family = (i - display_end) / 2;
            operation = (XrLirArrayOperation)(XR_LIR_ARRAY_OP_CAPACITY + (i - display_end) % 2);
        } else {
            family = i - capacity_end;
            operation = XR_LIR_ARRAY_OP_WITH_CAPACITY;
        }
        lir_array_catalog_signature(&parts[i], &entries[count + i], next, lir_array_elements[family], operation, reference);
    }
    const XrLirArrayOperation inline_operations[] = {
        XR_LIR_ARRAY_OP_NEW, XR_LIR_ARRAY_OP_LEN, XR_LIR_ARRAY_OP_GET, XR_LIR_ARRAY_OP_PUSH
    };
    uint32_t row = catalog, ordinal = 0;
    for (uint32_t t = 0; row < rows && t < m->type_count; ++t) {
        if (!m->types[t].inline_array_ordinal) continue;
        m->types[t].inline_array_ordinal = ++ordinal;
        XrLirType element = XR_LIR_TYPE_COUNT + t;
        size_t operation_count = xr_lir_class_array_push_element(m, element) ?
            sizeof(inline_operations) / sizeof(inline_operations[0]) : XR_LIR_ARRAY_OP_GET;
        for (size_t op = 0; op < operation_count; ++op, ++row) {
            while (used[next]) ++next;
            used[next] = 1;
            lir_array_catalog_signature(&parts[row], &entries[count + row], next, element, inline_operations[op], false);
        }
    }
    XR_CHECK(row == rows, "every required inline Array element has one catalog row per admitted operation");
    XrLirRegistry registry = {entries, total};
    XrLirStatus status = xr_lir_set_registry(m, &registry);
    for (uint32_t i = 0; status == XR_LIR_OK && i < rows; ++i)
        status = xr_lir_set_array_intrinsic(m, &parts[i].description);
    xr_free(entries); xr_free(used);
    if (parts != fixed) xr_free(parts);
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

XR_FUNC XrLirStatus xr_lir_set_array_held(XrLirModule *m, XrLirValue value) {
    XrLirStatus status = lir_writable(m);
    if (status != XR_LIR_OK) return status;
    if (value.module != m || !value.function || value.function > m->function_count)
        return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
    XrLirFunctionData *f = &m->functions[value.function - 1];
    if (!value.id || value.id > f->value_count) return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
    XrLirValueData *v = &f->values[value.id - 1];
    if (!v->parameter || !v->block || v->type != XR_LIR_ARRAY || !xr_lir_array_holdable(v->array_element))
        return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
    v->held = true;
    return XR_LIR_OK;
}

XR_FUNC bool xr_lir_array_held(const XrLirModule *m, XrLirValue value) {
    if (!xr_lir_module_is_readable(m) || value.module != m || !value.function || value.function > m->function_count)
        return false;
    const XrLirFunctionData *f = &m->functions[value.function - 1];
    return value.id && value.id <= f->value_count && f->values[value.id - 1].held;
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
    if (input && v->op == XR_LIR_NULLABLE_UNWRAP && v->type == XR_LIR_ARRAY)
        return xr_lir_nullable_array_element(m, input->type);
    if (input && (v->op == XR_LIR_STATE_READ ||
        (v->op == XR_LIR_ARRAY_BORROW && input->type == XR_LIR_STATE_REF)))
        return input->state_reference.array_element;
    if (input && (v->op == XR_LIR_RECORD_FIELD || v->op == XR_LIR_CLASS_FIELD)) {
        XrLirType row = input->type;
        const XrLirRecordField *fields = NULL;
        uint32_t count = 0;
        if (v->op == XR_LIR_CLASS_FIELD && !xr_lir_class_payload(m, row, &row)) return XR_LIR_TYPE_INVALID;
        if (xr_lir_aggregate_fields(m, row, &fields, &count) != XR_LIR_OK || v->bits >= count)
            return XR_LIR_TYPE_INVALID;
        return fields[v->bits].array_element;
    }
    if ((v->op == XR_LIR_CALL || v->op == XR_LIR_INVOKE) && v->callee && v->callee <= m->function_count)
        return m->functions[v->callee - 1].array_result;
    if (v->op == XR_LIR_CALL_INDIRECT && input) {
        XrLirCallableType signature;
        if (xr_lir_callable_type_view(m, input->type, &signature) == XR_LIR_OK)
            return signature.result_element;
    }
    if (v->op == XR_LIR_CHECKED_EMPTY_ARRAY) {
        const XrLirArrayIntrinsic *shape = v->bits <= UINT32_MAX
            ? xr_lir_internal_array_intrinsic(m, (uint32_t)v->bits) : NULL;
        return shape && shape->operation == XR_LIR_ARRAY_OP_NEW ? shape->result_element : XR_LIR_TYPE_INVALID;
    }
    if (v->op == XR_LIR_CALL_INTRINSIC) {
        XrLirSliceIntrinsic slice;
        if (xr_lir_slice_intrinsic_view(m, v->intrinsic, &slice) == XR_LIR_OK &&
            slice.operation == XR_LIR_SLICE_OP_COPY) return slice.element;
        uint32_t index = lir_array_intrinsic_index(m, v->intrinsic);
        if (index == UINT32_MAX) return XR_LIR_TYPE_INVALID;
        const XrLirIntrinsic *entry = &m->intrinsics[index];
        XrLirType snapshot = xr_lir_map_snapshot_element(entry->operation);
        if (snapshot && xr_lir_map_intrinsic_valid(m, entry)) return snapshot;
        return m->array_intrinsics ? m->array_intrinsics[index].result_element : XR_LIR_TYPE_INVALID;
    }
    if (input && (v->op == XR_LIR_CELL_NEW || v->op == XR_LIR_CELL_READ || v->op == XR_LIR_ARRAY_RETAIN ||
        v->op == XR_LIR_ARRAY_BORROW || v->op == XR_LIR_ARRAY_REF_READ || v->op == XR_LIR_ARRAY_HOLD ||
        v->op == XR_LIR_ARRAY_UNHOLD))
        return input->array_element;
    return XR_LIR_TYPE_INVALID;
}

XR_FUNC const XrLirArrayIntrinsic *xr_lir_internal_array_intrinsic(const XrLirModule *m,
    uint32_t id) {
    uint32_t index = lir_array_intrinsic_index(m, id);
    return index != UINT32_MAX && m->array_intrinsics && m->array_intrinsics[index].id ?
        &m->array_intrinsics[index] : NULL;
}
