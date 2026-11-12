/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_types.inc.c - Interning and receiving physical type shapes
 *
 * KEY CONCEPT:
 *   Type metadata belongs to the SSA owner. Hash tables contain ordinals,
 *   and growth never leaves pointers into an old descriptor vector.
 */
static bool lir_fixed_shape(XrLirTypeView shape) {
    return shape.kind == XR_LIR_TYPE_FIXED_ARRAY && shape.element == XR_LIR_U8 &&
        shape.count && shape.count <= 65535;
}

XR_FUNC uint32_t xr_lir_constructed_type_count(const XrLirModule *m) {
    return xr_lir_module_is_readable(m) ? m->type_count : 0;
}

static uint32_t lir_type_hash(XrLirTypeView shape) {
    uint32_t fields[] = {(uint32_t)shape.kind, shape.element, shape.count};
    return xr_hash_bytes(fields, sizeof(fields));
}

static uint32_t lir_type_slot(const XrLirModule *m, XrLirTypeView shape) {
    uint32_t slot = lir_type_hash(shape) & (m->type_index_capacity - 1);
    while (m->type_index[slot]) {
        XrLirTypeView found = m->types[m->type_index[slot] - 1];
        if (found.kind == shape.kind && found.element == shape.element && found.count == shape.count)
            break;
        slot = (slot + 1) & (m->type_index_capacity - 1);
    }
    return slot;
}

static bool lir_type_index_grow(XrLirModule *m) {
    uint64_t next = m->type_index_capacity ? (uint64_t)m->type_index_capacity * 2 : 16;
    if (next > UINT32_MAX || next > SIZE_MAX / sizeof(*m->type_index)) {
        lir_fail(m, XR_LIR_OUT_OF_MEMORY);
        return false;
    }
    uint32_t *index = lir_allocate(m, (size_t)next, sizeof(*index));
    if (!index) return false;
    memset(index, 0, (size_t)next * sizeof(*index));
    for (uint32_t n = 0; n < m->type_count; ++n) {
        uint32_t slot = lir_type_hash(m->types[n]) & ((uint32_t)next - 1);
        while (index[slot]) slot = (slot + 1) & ((uint32_t)next - 1);
        index[slot] = n + 1;
    }
    xr_free(m->type_index);
    m->type_index = index;
    m->type_index_capacity = (uint32_t)next;
    return true;
}

XR_FUNC bool xr_lir_internal_types_valid(const XrLirModule *m) {
    if (m->type_count > m->type_capacity || m->type_count > UINT32_MAX - XR_LIR_TYPE_COUNT ||
        (m->type_capacity && !m->types) || (m->type_index_capacity && !m->type_index) ||
        (m->type_index_capacity & (m->type_index_capacity - 1)) ||
        m->type_count > m->type_index_capacity / 2) return false;
    uint32_t entries = 0;
    for (uint32_t s = 0; s < m->type_index_capacity; ++s) {
        if (m->type_index[s] > m->type_count) return false;
        entries += m->type_index[s] != 0;
    }
    if (entries != m->type_count) return false;
    for (uint32_t n = 0; n < m->type_count; ++n)
        if (!lir_fixed_shape(m->types[n]) || m->type_index[lir_type_slot(m, m->types[n])] != n + 1)
            return false;
    return true;
}

XR_FUNC XrLirStatus xr_lir_fixed_array_type(XrLirModule *m, XrLirType element,
    uint32_t count, XrLirType *output) {
    XrLirStatus status = lir_writable(m);
    if (status != XR_LIR_OK) return status;
    XrLirTypeView shape = {XR_LIR_TYPE_FIXED_ARRAY, element, count};
    if (!output || !lir_fixed_shape(shape)) return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
    if (!m->type_index_capacity || m->type_count >= m->type_index_capacity / 2)
        if (!lir_type_index_grow(m)) return m->failure;
    uint32_t slot = lir_type_slot(m, shape);
    if (!m->type_index[slot]) {
        if (m->type_count >= UINT32_MAX - XR_LIR_TYPE_COUNT)
            return lir_fail(m, XR_LIR_OUT_OF_MEMORY);
        XrLirTypeView *grown = lir_reserve(m, m->types, m->type_count,
            &m->type_capacity, sizeof(*grown));
        if (!grown) return m->failure;
        m->types = grown;
        m->types[m->type_count] = shape;
        m->type_index[slot] = ++m->type_count;
    }
    *output = XR_LIR_TYPE_COUNT + m->type_index[slot] - 1;
    return XR_LIR_OK;
}

XR_FUNC XrLirStatus xr_lir_type_view(const XrLirModule *m, XrLirType type,
    XrLirTypeView *output) {
    if (!m || m->failure != XR_LIR_OK || !output || !xr_lir_internal_type_valid(m, type, true))
        return XR_LIR_INVALID_ARGUMENT;
    XrLirTypeView view = {XR_LIR_TYPE_BUILTIN, XR_LIR_TYPE_INVALID, 0};
    if (type >= XR_LIR_TYPE_COUNT) view = m->types[type - XR_LIR_TYPE_COUNT];
    *output = view;
    return XR_LIR_OK;
}

XR_FUNC XrLirStatus xr_lir_type_layout(const XrLirModule *m, XrLirType type,
    XrLirTypeLayout *output) {
    XrLirTypeView view;
    XrLirStatus status = xr_lir_type_view(m, type, &view);
    if (!output || status != XR_LIR_OK) return XR_LIR_INVALID_ARGUMENT;
    XrLirTypeLayout layout = {0, 1, 1, false};
    if (view.kind == XR_LIR_TYPE_FIXED_ARRAY) {
        layout.size = view.count;
        layout.scalar_count = view.count;
    } else {
        switch (type) {
        case XR_LIR_UNIT: layout.scalar_count = 0; break;
        case XR_LIR_I1: case XR_LIR_I8: case XR_LIR_U8: layout.size = 1; break;
        case XR_LIR_I16: case XR_LIR_U16: layout.size = 2; layout.alignment = 2; break;
        case XR_LIR_I32: case XR_LIR_U32: case XR_LIR_F32:
            layout.size = 4; layout.alignment = 4; break;
        case XR_LIR_I64: case XR_LIR_U64: case XR_LIR_F64: case XR_LIR_PTR:
            layout.size = 8; layout.alignment = 8; break;
        case XR_LIR_STRING: case XR_LIR_ARRAY:
            layout.size = 8; layout.alignment = 8; layout.managed = true; break;
        default: return XR_LIR_UNSUPPORTED;
        }
    }
    *output = layout;
    return XR_LIR_OK;
}

XR_FUNC XrLirStatus xr_lir_type_scalar(const XrLirModule *m, XrLirType type,
    uint32_t ordinal, XrLirScalarPart *output) {
    XrLirTypeLayout layout;
    XrLirStatus status = xr_lir_type_layout(m, type, &layout);
    if (status != XR_LIR_OK) return status;
    if (!output || ordinal >= layout.scalar_count) return XR_LIR_INVALID_ARGUMENT;
    *output = type < XR_LIR_TYPE_COUNT ? (XrLirScalarPart){type, 0} :
        (XrLirScalarPart){XR_LIR_U8, ordinal};
    return XR_LIR_OK;
}

XR_FUNC XrLirStatus xr_lir_import_type(XrLirModule *target, const XrLirModule *source,
    XrLirType type, XrLirType *output) {
    XrLirStatus status = lir_writable(target);
    if (status != XR_LIR_OK) return status;
    XrLirTypeView view;
    if (!output || xr_lir_type_view(source, type, &view) != XR_LIR_OK)
        return lir_fail(target, XR_LIR_INVALID_ARGUMENT);
    if (view.kind == XR_LIR_TYPE_FIXED_ARRAY)
        return xr_lir_fixed_array_type(target, view.element, view.count, output);
    *output = type;
    return XR_LIR_OK;
}

XR_FUNC XrLirStatus xr_lir_copy_types(XrLirModule *target, const XrLirModule *source) {
    XrLirStatus status = lir_writable(target);
    if (status != XR_LIR_OK) return status;
    if (!xr_lir_module_is_readable(source) || target->type_count)
        return lir_fail(target, XR_LIR_INVALID_ARGUMENT);
    for (uint32_t n = 0; n < source->type_count; ++n) {
        XrLirType imported = XR_LIR_TYPE_INVALID;
        status = xr_lir_import_type(target, source, XR_LIR_TYPE_COUNT + n, &imported);
        if (status != XR_LIR_OK) return status;
        XR_DCHECK(imported == XR_LIR_TYPE_COUNT + n, "a fresh receiving pool preserves canonical order");
    }
    return XR_LIR_OK;
}
