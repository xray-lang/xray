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
#include "xlir_map.h"
#include "xlir_pod.h"
#include "xlir_callable_types.inc.c"
#include "xlir_record_identity.inc.c"

static bool lir_fixed_shape(XrLirTypeView shape) {
    return shape.kind == XR_LIR_TYPE_FIXED_ARRAY && xr_lir_fixed_element_size(shape.element) &&
        shape.count && shape.count <= 65535;
}

XR_FUNC uint32_t xr_lir_constructed_type_count(const XrLirModule *m) {
    return xr_lir_module_is_readable(m) ? m->type_count : 0;
}

static bool lir_nullable_array_shape(XrLirTypeView shape) {
    return shape.kind == XR_LIR_TYPE_NULLABLE && shape.element == XR_LIR_ARRAY;
}

static uint32_t lir_type_hash(XrLirTypeView shape, const XrLirCallableType *callable,
    XrLirType cell_element, const XrLirRecordField *record, const XrLirRecordIdentity *identity,
    const XrLirSumVariant *variants) {
    uint32_t fields[] = {(uint32_t)shape.kind, shape.element, shape.count};
    uint32_t hash = xr_hash_bytes(fields, sizeof(fields));
    if (shape.kind == XR_LIR_TYPE_SUM && identity && identity->name)
        return lir_record_identity_hash(identity);
    if (shape.kind == XR_LIR_TYPE_TUPLE) {
        for (uint32_t f = 0; f < shape.count; ++f) {
            hash = (hash ^ record[f].type) * UINT32_C(16777619);
            hash = (hash ^ record[f].array_element) * UINT32_C(16777619);
        }
    }
    if (shape.kind == XR_LIR_TYPE_SUM && variants) {
        for (uint32_t v = 0; v < shape.count; ++v) {
            hash = (hash ^ variants[v].field_begin) * UINT32_C(16777619);
            hash = (hash ^ variants[v].field_count) * UINT32_C(16777619);
            for (uint32_t f = 0; f < variants[v].field_count; ++f) {
                hash = (hash ^ record[variants[v].field_begin + f].type) * UINT32_C(16777619);
                hash = (hash ^ record[variants[v].field_begin + f].array_element) * UINT32_C(16777619);
            }
        }
    }
    if (shape.kind == XR_LIR_TYPE_RECORD) return lir_record_identity_hash(identity);
    if (shape.kind == XR_LIR_TYPE_CELL || lir_nullable_array_shape(shape))
        hash ^= xr_hash_bytes(&cell_element, sizeof(cell_element));
    return shape.kind == XR_LIR_TYPE_CALLABLE ? lir_callable_hash(hash, callable) : hash;
}

static bool lir_tuple_fields_equal(const XrLirRecordField *left,
    const XrLirRecordField *right, uint32_t count) {
    for (uint32_t f = 0; f < count; ++f)
        if (left[f].type != right[f].type || left[f].array_element != right[f].array_element) return false;
    return true;
}

static bool lir_sum_shape_equal(const XrLirTypeData *found, XrLirTypeView shape,
    const XrLirRecordField *fields, const XrLirSumVariant *variants) {
    if (found->shape.count != shape.count || !variants || !found->variants ||
        found->variant_count != shape.count) return false;
    for (uint32_t v = 0; v < shape.count; ++v)
        if (found->variants[v].field_begin != variants[v].field_begin ||
            found->variants[v].field_count != variants[v].field_count) return false;
    for (uint32_t f = 0; f < shape.count; ++f) {
        XrLirSumVariant variant = variants[f];
        for (uint32_t n = 0; n < variant.field_count; ++n) {
            XrLirRecordField field = fields[variant.field_begin + n];
            if (found->fields[variant.field_begin + n].type != field.type ||
                found->fields[variant.field_begin + n].array_element != field.array_element) return false;
        }
    }
    return true;
}
static uint32_t lir_type_slot(const XrLirModule *m, XrLirTypeView shape,
    const XrLirCallableType *callable, XrLirType cell_element, const XrLirRecordField *record,
    const XrLirRecordIdentity *identity, const XrLirSumVariant *variants) {
    uint32_t slot = lir_type_hash(shape, callable, cell_element, record, identity, variants) & (m->type_index_capacity - 1);
    while (m->type_index[slot]) {
        const XrLirTypeData *found = &m->types[m->type_index[slot] - 1];
        if (shape.kind == XR_LIR_TYPE_RECORD && found->shape.kind == shape.kind &&
            lir_record_identity_equal(&found->record, identity)) break;
        if (shape.kind == XR_LIR_TYPE_SUM && found->shape.kind == shape.kind) {
            bool nominal = identity && identity->name;
            if (nominal ? found->record.name && found->record.flags == identity->flags &&
                    lir_record_identity_equal(&found->record, identity) :
                !found->record.name && lir_sum_shape_equal(found, shape, record, variants)) break;
        }
        if (shape.kind != XR_LIR_TYPE_RECORD && found->shape.kind == shape.kind && found->shape.element == shape.element &&
            found->shape.count == shape.count && found->cell_element == cell_element &&
            (shape.kind != XR_LIR_TYPE_TUPLE || lir_tuple_fields_equal(found->fields, record, shape.count)) &&
            shape.kind != XR_LIR_TYPE_SUM &&
            (shape.kind != XR_LIR_TYPE_CALLABLE || lir_callable_equal(&found->callable, callable)))
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
        uint32_t slot = lir_type_hash(m->types[n].shape, &m->types[n].callable,
            m->types[n].cell_element, m->types[n].fields, &m->types[n].record,
            m->types[n].variants) & ((uint32_t)next - 1);
        while (index[slot]) slot = (slot + 1) & ((uint32_t)next - 1);
        index[slot] = n + 1;
    }
    xr_free(m->type_index);
    m->type_index = index;
    m->type_index_capacity = (uint32_t)next;
    return true;
}

static bool lir_cell_payload_valid(const XrLirModule *m, XrLirType payload,
    XrLirType element, uint32_t ordinal) {
    if (!xr_lir_internal_type_valid(m, payload, true) || payload >= XR_LIR_TYPE_COUNT + ordinal) return false;
    if (payload == XR_LIR_ARRAY) return xr_lir_internal_array_family(element) != UINT32_MAX;
    if (element) return false;
    if (xr_lir_internal_slice_type(m, payload)) return false;
    if (payload < XR_LIR_TYPE_COUNT) return lir_pod_scalar(payload) < XR_POD_SCALAR_COUNT ||
        payload == XR_LIR_STRING || payload == XR_LIR_MAP_STRING_I64;
    /* Class field guards assume a single executor. Cells cannot transport
     * Class handles across that boundary. */
    while (payload >= XR_LIR_TYPE_COUNT) {
        XrLirTypeView shape = m->types[payload - XR_LIR_TYPE_COUNT].shape;
        if (shape.kind != XR_LIR_TYPE_NULLABLE) return shape.kind != XR_LIR_TYPE_CLASS;
        if (shape.element >= payload) return false;
        payload = shape.element;
    }
    return true;
}

static bool lir_type_element_valid(const XrLirModule *m, XrLirTypeView shape, XrLirType element, uint32_t ordinal) {
    if (shape.kind == XR_LIR_TYPE_CELL) return lir_cell_payload_valid(m, shape.element, element, ordinal);
    if (lir_nullable_array_shape(shape)) return element == XR_LIR_U8 || element == XR_LIR_I64 ||
        element == XR_LIR_STRING || element == XR_LIR_F64 ||
        (element >= XR_LIR_TYPE_COUNT && element - XR_LIR_TYPE_COUNT < ordinal &&
            xr_lir_internal_array_element(m, element));
    return element == XR_LIR_TYPE_INVALID;
}

static bool lir_tuple_shape(const XrLirModule *m, XrLirTypeView shape, const XrLirRecordField *fields,
    uint32_t ordinal, XrLirTypeLayout *output) {
    if (shape.element || !fields || !shape.count || shape.count > 65535) return false;
    bool managed = false;
    for (uint32_t f = 0; f < shape.count; ++f) {
        XrLirRecordField field = fields[f];
        if (field.name || field.name_length || field.flags || field.type >= XR_LIR_TYPE_COUNT + ordinal)
            return false;
        if (field.type == XR_LIR_ARRAY ? field.array_element != XR_LIR_I64 :
            field.array_element || !(field.type == XR_LIR_I64 || field.type == XR_LIR_I1 || field.type == XR_LIR_F64 ||
                field.type == XR_LIR_STRING || xr_lir_inline_aggregate_type(m, field.type))) return false;
        managed |= field.type == XR_LIR_STRING || field.type == XR_LIR_ARRAY || xr_lir_boxed_type(m, field.type);
    }
    /* Offsets and extent belong to the common inline plan. Ownership is also
     * needed before that plan exists to construct valid LIR edges. */
    *output = (XrLirTypeLayout){0, 1, 0, managed};
    return true;
}

static bool lir_sum_shape(const XrLirModule *m, XrLirTypeView shape,
    const XrLirRecordField *fields, const XrLirSumVariant *variants, uint32_t ordinal, XrLirTypeLayout *output) {
    if (shape.element || !shape.count || !variants) return false;
    uint32_t tag = xr_lir_sum_tag_size(shape.count);
    if (tag > 4) return false;
    size_t alignment = tag ? tag : 1, fields_seen = 0, maximum = 0;
    for (uint32_t v = 0; v < shape.count; ++v) {
        XrLirSumVariant variant = variants[v];
        if (variant.field_begin != fields_seen || variant.field_count > SIZE_MAX - fields_seen) return false;
        fields_seen += variant.field_count;
    }
    if (fields_seen > UINT32_MAX) return false;
    if (fields_seen && !fields) return false;
    uint32_t field_count = (uint32_t)fields_seen;
    for (uint32_t f = 0; f < field_count; ++f) {
        XrLirRecordField field = fields[f];
        if (field.flags || field.array_element ||
            (field.type < XR_LIR_TYPE_COUNT && field.type <= XR_LIR_UNIT)) return false;
        if (field.type >= XR_LIR_TYPE_COUNT && field.type >= XR_LIR_TYPE_COUNT + ordinal) return false;
        XrLirTypeLayout child = {0, 1, 0, false};
        if (field.type >= XR_LIR_TYPE_COUNT) {
            XrLirTypeKind kind = m->types[field.type - XR_LIR_TYPE_COUNT].shape.kind;
            if (kind == XR_LIR_TYPE_SLICE || kind == XR_LIR_TYPE_CALLABLE || kind == XR_LIR_TYPE_CELL) return false;
            child = m->types[field.type - XR_LIR_TYPE_COUNT].layout;
        } else {
            XrLirStatus st = xr_lir_type_layout(m, field.type, &child);
            if (st != XR_LIR_OK) return false;
        }
        if (!child.size || child.managed || !child.alignment) return false;
        if (child.alignment > alignment) alignment = child.alignment;
    }
    size_t payload = (tag + alignment - 1) & ~(alignment - 1);
    for (uint32_t v = 0; v < shape.count; ++v) {
        XrLirSumVariant variant = variants[v];
        size_t end = payload;
        for (uint32_t f = 0; f < variant.field_count; ++f) {
            XrLirTypeLayout child = {0, 1, 0, false};
            XrLirType type = fields[variant.field_begin + f].type;
            if (type >= XR_LIR_TYPE_COUNT) child = m->types[type - XR_LIR_TYPE_COUNT].layout;
            else {
                XrLirStatus st = xr_lir_type_layout(m, type, &child);
                if (st != XR_LIR_OK) return false;
            }
            if (end > SIZE_MAX - (child.alignment - 1)) return false;
            size_t offset = (end + child.alignment - 1) & ~((size_t)child.alignment - 1);
            if (child.size > SIZE_MAX - offset) return false;
            end = offset + child.size;
        }
        if (end > maximum) maximum = end;
    }
    if (maximum > SIZE_MAX - (alignment - 1)) return false;
    size_t size = (maximum + alignment - 1) & ~(alignment - 1);
    if (!size) size = 1;
    if (size > UINT32_MAX) return false;
    /* Owner-free by admission: no variant field owns a managed reference. */
    *output = (XrLirTypeLayout){size, (uint32_t)alignment, 0, false};
    return true;
}
static bool lir_shape_layout(const XrLirModule *m, XrLirTypeView shape, const XrLirRecordField *fields,
    const XrLirSumVariant *variants, uint32_t ordinal, XrLirTypeLayout *output) {
    if (shape.kind == XR_LIR_TYPE_SLICE) {
        if (shape.count || (shape.element != XR_LIR_I64 && shape.element != XR_LIR_U8)) return false;
        *output = (XrLirTypeLayout){sizeof(XrtReadonlyArraySlice), _Alignof(XrtReadonlyArraySlice), 0, false};
        return true;
    }
    if (lir_fixed_shape(shape)) {
        uint32_t width = xr_lir_fixed_element_size(shape.element);
        *output = (XrLirTypeLayout){shape.count * width, width, shape.count, false};
        return true;
    }
    if (shape.kind == XR_LIR_TYPE_SUM) return lir_sum_shape(m, shape, fields, variants, ordinal, output);
    if (shape.kind == XR_LIR_TYPE_TUPLE) return lir_tuple_shape(m, shape, fields, ordinal, output);
    if (shape.kind == XR_LIR_TYPE_RECORD) {
        if (shape.element || shape.count > 65536) return false;
        *output = (XrLirTypeLayout){sizeof(void *), _Alignof(void *), 0, true};
        return true;
    }
    if (shape.kind == XR_LIR_TYPE_CLASS) {
        if (shape.count || shape.element < XR_LIR_TYPE_COUNT || shape.element >= XR_LIR_TYPE_COUNT + ordinal ||
            m->types[shape.element - XR_LIR_TYPE_COUNT].shape.kind != XR_LIR_TYPE_RECORD) return false;
        const XrLirTypeData *row = &m->types[shape.element - XR_LIR_TYPE_COUNT];
        if (row->record.native_id == XR_NATIVE_DECLARATION_FILE_STORAGE && row->shape.count) return false;
        *output = (XrLirTypeLayout){sizeof(void *), _Alignof(void *), 0, true};
        return true;
    }
    if (shape.kind == XR_LIR_TYPE_CALLABLE) {
        *output = (XrLirTypeLayout){0, 1, 0, true};
        return true;
    }
    if (shape.kind == XR_LIR_TYPE_CELL) {
        if (shape.count || shape.element >= XR_LIR_TYPE_COUNT + ordinal) return false;
        *output = (XrLirTypeLayout){sizeof(void *), _Alignof(void *), 0, true};
        return true;
    }
    if (shape.kind != XR_LIR_TYPE_NULLABLE || shape.count ||
        shape.element >= XR_LIR_TYPE_COUNT + ordinal) return false;
    if (shape.element >= XR_LIR_TYPE_COUNT) {
        XrLirTypeData child = m->types[shape.element - XR_LIR_TYPE_COUNT];
        if (!child.layout.size && xr_lir_inline_aggregate_type(m, shape.element)) {
            *output = child.layout;
            return true;
        }
    }
    if (shape.element == XR_LIR_STRING || lir_nullable_array_shape(shape) || xr_lir_boxed_type(m, shape.element)) {
        *output = (XrLirTypeLayout){sizeof(void *), _Alignof(void *), 0, true};
        return true;
    }
    XrPodLayout payload;
    if (shape.element < XR_LIR_TYPE_COUNT) {
        if (!xr_pod_scalar_layout(lir_pod_scalar(shape.element), &payload)) return false;
    } else {
        XrLirTypeLayout child = m->types[shape.element - XR_LIR_TYPE_COUNT].layout;
        if (child.managed || child.size > UINT32_MAX || xr_lir_internal_slice_type(m, shape.element)) return false;
        payload = (XrPodLayout){(uint32_t)child.size, child.alignment, child.scalar_count};
    }
    XrPodLayout layout;
    if (!xr_pod_nullable_layout(payload, &layout)) return false;
    *output = (XrLirTypeLayout){layout.size, layout.alignment, layout.scalar_count, false};
    return true;
}

static bool lir_record_fields_valid(const XrLirModule *m, const XrLirRecordField *fields,
    uint32_t count, uint32_t earlier, bool allow_class_fields, bool *class_payload_only) {
    if ((count != 0) != (fields != NULL)) return false;
    for (uint32_t f = 0; f < count; ++f) {
        XrLirRecordField field = fields[f];
        bool class_field = allow_class_fields && xr_lir_internal_class_only_field(m, field.type);
        if (!lir_record_text_valid(field.name,field.name_length) ||
            (field.flags & ~(XR_LIR_RECORD_PRIVATE|XR_LIR_RECORD_PROTECTED|XR_LIR_RECORD_MUTABLE)) ||
            ((field.flags & (XR_LIR_RECORD_PRIVATE|XR_LIR_RECORD_PROTECTED)) == (XR_LIR_RECORD_PRIVATE|XR_LIR_RECORD_PROTECTED)) ||
            field.type >= XR_LIR_TYPE_COUNT + earlier ||
            (field.type == XR_LIR_ARRAY ? !xr_lir_class_array_element_supported(field.array_element) :
                field.array_element || !(field.type == XR_LIR_STRING || field.type == XR_LIR_I64 ||
                    field.type == XR_LIR_I1 || field.type == XR_LIR_F64 ||
                    xr_lir_inline_aggregate_type(m, field.type) || xr_lir_class_payload(m, field.type, NULL) ||
                    class_field))) return false;
        *class_payload_only |= class_field;
    }
    return true;
}

enum { LIR_SUM_PAYLOADLESS_ENUM = 1u << 1 };

static bool lir_sum_names_valid(const XrLirModule *m, XrLirTypeView shape, const XrLirRecordIdentity *identity,
    const XrLirRecordField *fields, const XrLirSumVariant *variants, uint32_t earlier) {
    bool nominal = identity && identity->name;
    bool payloadless = nominal && identity->flags == LIR_SUM_PAYLOADLESS_ENUM;
    if (nominal) {
        XrLirRecordIdentity key = *identity;
        key.flags = 0;
        if (key.native_id || key.argument_count || (identity->flags && !payloadless) ||
            !lir_record_identity_valid(m, &key, earlier)) return false;
    } else if (identity && identity->flags) return false;
    for (uint32_t v = 0; v < shape.count; ++v) {
        XrLirSumVariant variant = variants[v];
        if (payloadless && variant.field_count) return false;
        if (nominal ? !lir_record_text_valid(variant.name, variant.name_length) :
            variant.name || variant.name_length) return false;
        for (uint32_t n = 0; n < variant.field_count; ++n) {
            XrLirRecordField field = fields[variant.field_begin + n];
            if (nominal ? field.type >= XR_LIR_TYPE_COUNT || lir_pod_scalar(field.type) >= XR_POD_SCALAR_COUNT ||
                    !lir_record_text_valid(field.name, field.name_length) :
                field.name || field.name_length) return false;
        }
    }
    return true;
}

static bool lir_named_sum_equal(const XrLirTypeData *found, XrLirTypeView shape,
    const XrLirRecordIdentity *identity, const XrLirRecordField *fields, const XrLirSumVariant *variants,
    uint32_t field_count) {
    if (!lir_sum_shape_equal(found, shape, fields, variants) ||
        found->record.exported != identity->exported || found->record.flags != identity->flags ||
        !lir_record_schema_equal(found->fields, fields, field_count, true)) return false;
    for (uint32_t v = 0; v < shape.count; ++v)
        if (found->variants[v].name_length != variants[v].name_length ||
            memcmp(found->variants[v].name, variants[v].name, variants[v].name_length)) return false;
    return true;
}

static bool lir_class_parent_valid(const XrLirModule *m, XrLirTypeView shape, XrLirType parent, uint32_t earlier) {
    if (!parent) return true;
    if (shape.kind != XR_LIR_TYPE_CLASS || parent < XR_LIR_TYPE_COUNT ||
        parent >= XR_LIR_TYPE_COUNT + earlier) return false;
    const XrLirTypeData *base = &m->types[parent - XR_LIR_TYPE_COUNT];
    if (base->shape.kind != XR_LIR_TYPE_CLASS || base->shape.element < XR_LIR_TYPE_COUNT ||
        base->shape.element >= parent || base->shape.element == shape.element) return false;
    const XrLirTypeData *inherited = &m->types[base->shape.element - XR_LIR_TYPE_COUNT];
    const XrLirTypeData *payload = &m->types[shape.element - XR_LIR_TYPE_COUNT];
    return !inherited->record.native_id && !payload->record.native_id &&
        inherited->shape.count <= payload->shape.count &&
        lir_record_schema_equal(inherited->fields, payload->fields, inherited->shape.count, true);
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
    for (uint32_t n = 0; n < m->type_count; ++n) {
        XrLirTypeData type = m->types[n];
        XrLirTypeLayout expected;
        bool class_payload_only = false;
        if (!lir_shape_layout(m, type.shape, type.fields, type.variants, n, &expected) ||
            !lir_class_parent_valid(m, type.shape, type.class_parent, n) ||
            expected.size != type.layout.size || expected.alignment != type.layout.alignment ||
            expected.scalar_count != type.layout.scalar_count || expected.managed != type.layout.managed ||
            (type.shape.kind == XR_LIR_TYPE_CALLABLE &&
                (type.shape.element != type.callable.result || type.shape.count != type.callable.parameter_count ||
                    !lir_callable_signature_valid(m, &type.callable, n))) ||
            (type.shape.kind != XR_LIR_TYPE_CALLABLE && type.callable.parameters) ||
            (type.shape.kind == XR_LIR_TYPE_RECORD ?
                (!lir_record_fields_valid(m, type.fields, type.shape.count, n, type.class_payload_only,
                     &class_payload_only) || !lir_record_identity_valid(m,&type.record,n) ||
                 !lir_record_payload_identity_valid(m, &type.record, type.fields, type.shape.count, class_payload_only) ||
                 !lir_record_name_index_valid(&type)) :
                ((type.shape.kind != XR_LIR_TYPE_TUPLE && type.shape.kind != XR_LIR_TYPE_SUM && type.fields) ||
                 (type.shape.kind != XR_LIR_TYPE_SUM && (
                 type.record.module || type.record.name || type.record.arguments || type.record.module_length ||
                 type.record.name_length || type.record.argument_count || type.record.exported || type.record.flags)) ||
                 type.record_names || type.record_name_capacity)) ||
            (type.shape.kind == XR_LIR_TYPE_SUM ?
                (!type.variants || type.variant_count != type.shape.count ||
                 !lir_sum_names_valid(m,type.shape,&type.record,type.fields,type.variants,n)) :
                (type.variants || type.variant_count)) ||
            class_payload_only != type.class_payload_only ||
            !lir_type_element_valid(m, type.shape, type.cell_element, n) ||
            m->type_index[lir_type_slot(m, type.shape, &type.callable, type.cell_element, type.fields,
                &type.record, type.variants)] != n + 1) return false;
    }
    for (uint32_t n=0;n<m->type_count;++n) {
        XrLirTypeData type=m->types[n];
        if(type.shape.kind==XR_LIR_TYPE_RECORD &&
            !lir_record_declaration_consistent(m,&type.record,type.fields,type.shape.count))return false;
    }
    return true;
}

static bool lir_inline_display_member(const XrLirModule *m, XrLirType type) {
    if (type < XR_LIR_TYPE_COUNT)
        return type == XR_LIR_STRING || type == XR_LIR_I64 || type == XR_LIR_F64 || type == XR_LIR_I1;
    return m->types[type - XR_LIR_TYPE_COUNT].inline_display;
}

static bool lir_inline_display_shape(const XrLirModule *m, XrLirTypeView shape, const XrLirRecordField *fields) {
    if (shape.kind == XR_LIR_TYPE_NULLABLE) return lir_inline_display_member(m, shape.element);
    if (shape.kind != XR_LIR_TYPE_TUPLE) return false;
    for (uint32_t i = 0; i < shape.count; ++i)
        if (!lir_inline_display_member(m, fields[i].type)) return false;
    return true;
}

typedef struct LirNominalType {
    const XrLirRecordIdentity *identity;
    bool allow_class_fields;
} LirNominalType;

static XrLirStatus lir_intern_type(XrLirModule *m, XrLirTypeView shape,
    const XrLirCallableType *callable, XrLirType cell_element, XrLirType class_parent, const XrLirRecordField *fields,
    LirNominalType nominal, const XrLirSumVariant *variants, XrLirType *output) {
    const XrLirRecordIdentity *record = nominal.identity;
    XrLirStatus status = lir_writable(m);
    if (status != XR_LIR_OK) return status;
    XrLirTypeLayout layout;
    uint32_t field_count = 0;
    if (shape.kind == XR_LIR_TYPE_SUM && variants)
        for (uint32_t v = 0; v < shape.count; ++v) {
            if (variants[v].field_count > UINT32_MAX - field_count) return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
            field_count += variants[v].field_count;
        }
    else field_count = shape.count;
    if (!output || !lir_shape_layout(m, shape, fields, variants, m->type_count, &layout) ||
        !lir_class_parent_valid(m, shape, class_parent, m->type_count) ||
        (shape.kind == XR_LIR_TYPE_CALLABLE && !lir_callable_signature_valid(m, callable, m->type_count)) ||
        !lir_type_element_valid(m, shape, cell_element, m->type_count))
        return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
    bool class_payload_only = false;
    if (shape.kind == XR_LIR_TYPE_RECORD) {
        if (!lir_record_fields_valid(m, fields, shape.count, m->type_count, nominal.allow_class_fields,
                &class_payload_only) ||
            !lir_record_identity_valid(m,record,m->type_count) ||
            !lir_record_payload_identity_valid(m, record, fields, shape.count, class_payload_only) ||
            !lir_record_declaration_consistent(m,record,fields,shape.count)) return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
    } else if (nominal.allow_class_fields) {
        return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
    } else if (shape.kind == XR_LIR_TYPE_SUM) {
        if (!lir_sum_names_valid(m,shape,record,fields,variants,m->type_count))
            return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
    } else if (record || variants || (fields && shape.kind != XR_LIR_TYPE_TUPLE))
        return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
    if (!m->type_index_capacity || m->type_count >= m->type_index_capacity / 2)
        if (!lir_type_index_grow(m)) return m->failure;
    uint32_t slot = lir_type_slot(m, shape, callable, cell_element, fields, record, variants);
    if (m->type_index[slot]) {
        const XrLirTypeData *found = &m->types[m->type_index[slot] - 1];
        if (found->class_parent != class_parent || found->class_payload_only != class_payload_only)
            return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
    }
    if(m->type_index[slot] && shape.kind==XR_LIR_TYPE_RECORD &&
        !lir_record_schema_equal(m->types[m->type_index[slot]-1].fields,fields,shape.count,true))
        return lir_fail(m,XR_LIR_INVALID_ARGUMENT);
    if (m->type_index[slot] && shape.kind == XR_LIR_TYPE_SUM && record &&
        !lir_named_sum_equal(&m->types[m->type_index[slot]-1],shape,record,fields,variants,field_count))
        return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
    if (!m->type_index[slot]) {
        if (m->type_count >= UINT32_MAX - XR_LIR_TYPE_COUNT)
            return lir_fail(m, XR_LIR_OUT_OF_MEMORY);
        XrLirTypeData *grown = lir_reserve(m, m->types, m->type_count, &m->type_capacity, sizeof(*grown));
        if (!grown) return m->failure;
        m->types = grown;
        XrLirCallableType received = {0};
        if (callable) {
            received = *callable;
            received.parameters = callable->parameter_count ? lir_copy(m, callable->parameters,
                callable->parameter_count, sizeof(*callable->parameters)) : NULL;
            if (callable->parameter_count && !received.parameters) return m->failure;
        }
        XrLirRecordField *owned_fields = NULL;
        if (fields && field_count) {
            owned_fields = lir_copy(m, fields, field_count, sizeof(*fields));
            if (!owned_fields) return m->failure;
            if (record) {
                for (uint32_t f = 0; f < field_count; ++f) owned_fields[f].name = NULL;
                for (uint32_t f = 0; f < field_count; ++f) {
                    owned_fields[f].name = lir_copy(m, fields[f].name, fields[f].name_length, 1);
                    if (!owned_fields[f].name) {
                        for (uint32_t k = 0; k < f; ++k) xr_free((void *)owned_fields[k].name);
                        xr_free(owned_fields);
                        return m->failure;
                    }
                }
            }
        }
        XrLirSumVariant *owned_variants = NULL;
        if (variants) {
            owned_variants = lir_copy(m, variants, shape.count, sizeof(*variants));
            if (!owned_variants) {
                goto nominal_copy_failed;
            }
            if (record)
                for (uint32_t v = 0; v < shape.count; ++v) {
                    owned_variants[v].name = lir_record_text_copy(m, variants[v].name, variants[v].name_length);
                    if (!owned_variants[v].name) {
                        for (uint32_t k = 0; k < v; ++k) xr_free((void *)owned_variants[k].name);
                        xr_free(owned_variants);
                        goto nominal_copy_failed;
                    }
                }
        }
        XrLirRecordIdentity owned_record={0};uint32_t *names=NULL,name_capacity=0;
        if(record &&
            (!lir_record_identity_copy(m,record,&owned_record) ||
             (shape.kind == XR_LIR_TYPE_RECORD && !lir_record_name_index(m,owned_fields,shape.count,&names,&name_capacity)))) {
            lir_record_identity_free(&owned_record);
            if (owned_variants)
                for (uint32_t v = 0; v < shape.count; ++v) xr_free((void *)owned_variants[v].name);
            xr_free(owned_variants);
            goto nominal_copy_failed;
        }
        m->types[m->type_count] = (XrLirTypeData){.shape=shape, .layout=layout, .callable=received,
            .cell_element=cell_element, .class_parent=class_parent, .fields=owned_fields,
            .variants=owned_variants, .variant_count=shape.kind==XR_LIR_TYPE_SUM ? shape.count : 0,
            .record=owned_record,
            .record_names=names,.record_name_capacity=name_capacity,
            .inline_display=lir_inline_display_shape(m,shape,owned_fields), .class_payload_only=class_payload_only};
        m->type_index[slot] = ++m->type_count;
        *output = XR_LIR_TYPE_COUNT + m->type_index[slot] - 1;
        return XR_LIR_OK;
nominal_copy_failed:
        if (record && owned_fields)
            for (uint32_t f = 0; f < field_count; ++f) xr_free((void *)owned_fields[f].name);
        xr_free(owned_fields);
        xr_free((void *)received.parameters);
        return m->failure;
    }
    *output = XR_LIR_TYPE_COUNT + m->type_index[slot] - 1;
    return XR_LIR_OK;
}

XR_FUNC XrLirStatus xr_lir_fixed_array_type(XrLirModule *m, XrLirType element,
    uint32_t count, XrLirType *output) {
    return lir_intern_type(m, (XrLirTypeView){XR_LIR_TYPE_FIXED_ARRAY, element, count}, NULL, 0, 0, NULL, (LirNominalType){0}, NULL, output);
}

XR_FUNC XrLirStatus xr_lir_nullable_type(XrLirModule *m, XrLirType element, XrLirType *output) {
    return lir_intern_type(m, (XrLirTypeView){XR_LIR_TYPE_NULLABLE, element, 0}, NULL, 0, 0, NULL, (LirNominalType){0}, NULL, output);
}
XR_FUNC XrLirStatus xr_lir_nullable_array_type(XrLirModule *m, XrLirType element, XrLirType *output) {
    return lir_intern_type(m, (XrLirTypeView){XR_LIR_TYPE_NULLABLE, XR_LIR_ARRAY, 0}, NULL, element, 0, NULL, (LirNominalType){0}, NULL, output);
}
XR_FUNC XrLirType xr_lir_nullable_array_element(const XrLirModule *m, XrLirType type) {
    XrLirTypeView shape;
    if (xr_lir_type_view(m, type, &shape) != XR_LIR_OK || !lir_nullable_array_shape(shape)) return XR_LIR_TYPE_INVALID;
    return m->types[type - XR_LIR_TYPE_COUNT].cell_element;
}
XR_FUNC XrLirStatus xr_lir_tuple_type(XrLirModule *m, const XrLirRecordField *fields,
    uint32_t count, XrLirType *output) {
    return lir_intern_type(m, (XrLirTypeView){XR_LIR_TYPE_TUPLE, 0, count}, NULL, 0, 0, fields, (LirNominalType){0}, NULL, output);
}
XR_FUNC XrLirStatus xr_lir_tuple_i64_type(XrLirModule *m, uint32_t count, XrLirType *output) {
    XrLirStatus status = lir_writable(m);
    if (status != XR_LIR_OK) return status;
    if (!count || count > 65535 || !output) return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
    XrLirRecordField *fields = lir_allocate(m, count, sizeof(*fields));
    if (!fields) return m->failure;
    for (uint32_t f = 0; f < count; ++f) fields[f] = (XrLirRecordField){.type=XR_LIR_I64};
    status = xr_lir_tuple_type(m, fields, count, output);
    xr_free(fields);
    return status;
}

static XrLirStatus lir_sum_type(XrLirModule *m, const XrLirRecordIdentity *identity, const XrLirRecordField *fields,
    uint32_t field_count, const XrLirSumVariant *variants, uint32_t variant_count, XrLirType *output) {
    if (!variant_count || !variants || (field_count && !fields)) return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
    uint32_t total = 0;
    for (uint32_t v = 0; v < variant_count; ++v) {
        if (variants[v].field_count > UINT32_MAX - total) return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
        total += variants[v].field_count;
    }
    if (total != field_count) return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
    return lir_intern_type(m, (XrLirTypeView){XR_LIR_TYPE_SUM, 0, variant_count}, NULL, 0, 0,
        fields, (LirNominalType){.identity=identity}, variants, output);
}
XR_FUNC XrLirStatus xr_lir_sum_type(XrLirModule *m, const XrLirRecordField *fields, uint32_t field_count,
    const XrLirSumVariant *variants, uint32_t variant_count, XrLirType *output) {
    return lir_sum_type(m,NULL,fields,field_count,variants,variant_count,output);
}
XR_FUNC XrLirStatus xr_lir_error_sum_type(XrLirModule *m, const XrLirRecordIdentity *identity,
    const XrLirRecordField *fields, uint32_t field_count, const XrLirSumVariant *variants,
    uint32_t variant_count, XrLirType *output) {
    if (!identity || !identity->name || identity->argument_count || identity->flags)
        return lir_fail(m,XR_LIR_INVALID_ARGUMENT);
    return lir_sum_type(m,identity,fields,field_count,variants,variant_count,output);
}
XR_FUNC XrLirStatus xr_lir_internal_payloadless_enum_type(XrLirModule *m, const XrLirRecordIdentity *identity,
    const XrLirSumVariant *variants, uint32_t variant_count, XrLirType *output) {
    if (!identity || !identity->name || identity->argument_count || identity->flags)
        return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
    XrLirRecordIdentity key = *identity;
    key.flags = LIR_SUM_PAYLOADLESS_ENUM;
    return lir_sum_type(m, &key, NULL, 0, variants, variant_count, output);
}
XR_FUNC bool xr_lir_error_sum_identity(const XrLirModule *m, XrLirType type, XrLirRecordIdentity *output) {
    if (!output || type < XR_LIR_TYPE_COUNT || !xr_lir_internal_type_valid(m,type,true)) return false;
    const XrLirTypeData *data = &m->types[type - XR_LIR_TYPE_COUNT];
    if (data->shape.kind != XR_LIR_TYPE_SUM || !data->record.name || data->record.flags) return false;
    *output = data->record;
    return true;
}
XR_FUNC bool xr_lir_sum_variants(const XrLirModule *m, XrLirType type,
    const XrLirRecordField **fields, uint32_t *field_count,
    const XrLirSumVariant **variants, uint32_t *variant_count) {
    XrLirTypeView shape;
    if (!fields || !field_count || !variants || !variant_count ||
        xr_lir_type_view(m, type, &shape) != XR_LIR_OK || shape.kind != XR_LIR_TYPE_SUM)
        return false;
    const XrLirTypeData *data = &m->types[type - XR_LIR_TYPE_COUNT];
    *fields = data->fields; *variants = data->variants;
    *variant_count = data->variant_count;
    uint32_t total = 0;
    for (uint32_t v = 0; v < data->variant_count; ++v) total += data->variants[v].field_count;
    *field_count = total;
    return true;
}

XR_FUNC XrLirStatus xr_lir_record_type(XrLirModule *m, const XrLirRecordIdentity *identity, const XrLirRecordField *fields,
    uint32_t count, XrLirType *output) {
    return lir_intern_type(m, (XrLirTypeView){XR_LIR_TYPE_RECORD, 0, count}, NULL, 0, 0, fields,
        (LirNominalType){.identity=identity}, NULL, output);
}

XR_FUNC XrLirStatus xr_lir_internal_class_type(XrLirModule *m, XrLirType payload, XrLirType parent, XrLirType *output) {
    return lir_intern_type(m, (XrLirTypeView){XR_LIR_TYPE_CLASS, payload, 0}, NULL, 0, parent, NULL, (LirNominalType){0}, NULL, output);
}

XR_FUNC XrLirStatus xr_lir_internal_named_class_type(XrLirModule *m, const XrLirRecordIdentity *identity,
    const XrLirRecordField *fields, uint32_t count, XrLirType parent, XrLirType *output) {
    if (!output) return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
    XrLirType payload = XR_LIR_TYPE_INVALID;
    XrLirStatus status = lir_intern_type(m, (XrLirTypeView){XR_LIR_TYPE_RECORD, 0, count}, NULL, 0, 0, fields,
        (LirNominalType){.identity=identity, .allow_class_fields=true}, NULL, &payload);
    return status == XR_LIR_OK ? xr_lir_internal_class_type(m, payload, parent, output) : status;
}

XR_FUNC bool xr_lir_class_payload(const XrLirModule *m, XrLirType type, XrLirType *output) {
    XrLirTypeView shape;
    if (xr_lir_type_view(m, type, &shape) != XR_LIR_OK || shape.kind != XR_LIR_TYPE_CLASS) return false;
    if (output) *output = shape.element;
    return true;
}

XR_FUNC XrLirType xr_lir_class_parent(const XrLirModule *m, XrLirType type) {
    return xr_lir_class_payload(m, type, NULL) ? m->types[type - XR_LIR_TYPE_COUNT].class_parent : XR_LIR_TYPE_INVALID;
}
XR_FUNC uint32_t xr_lir_class_storage(const XrLirModule *m, XrLirType type) {
    XrLirType payload;
    return xr_lir_class_payload(m, type, &payload) ? m->types[payload - XR_LIR_TYPE_COUNT].record.native_id : 0;
}
XR_FUNC XrLirStatus xr_lir_slice_type(XrLirModule *m, XrLirType element, XrLirType *output) {
    return lir_intern_type(m, (XrLirTypeView){XR_LIR_TYPE_SLICE, element, 0}, NULL, 0, 0, NULL, (LirNominalType){0}, NULL, output);
}
XR_FUNC XrLirStatus xr_lir_aggregate_fields(const XrLirModule *m, XrLirType type,
    const XrLirRecordField **fields, uint32_t *count) {
    XrLirTypeView shape;
    if (!fields || !count || xr_lir_type_view(m, type, &shape) != XR_LIR_OK ||
        (shape.kind != XR_LIR_TYPE_RECORD && shape.kind != XR_LIR_TYPE_TUPLE))
        return XR_LIR_INVALID_ARGUMENT;
    *fields = m->types[type - XR_LIR_TYPE_COUNT].fields;
    *count = shape.count;
    return XR_LIR_OK;
}
XR_FUNC XrLirStatus xr_lir_record_identity(const XrLirModule *m,XrLirType type,XrLirRecordIdentity *output) {
    XrLirTypeView shape;
    if(!output || xr_lir_type_view(m,type,&shape)!=XR_LIR_OK || shape.kind!=XR_LIR_TYPE_RECORD)
        return XR_LIR_INVALID_ARGUMENT;
    *output=m->types[type-XR_LIR_TYPE_COUNT].record;return XR_LIR_OK;
}

XR_FUNC bool xr_lir_boxed_type(const XrLirModule *m, XrLirType type) {
    const XrLirTypeData *data = xr_lir_internal_constructed_data(m, type);
    if (!data || data->class_payload_only) return false;
    return data->shape.kind == XR_LIR_TYPE_RECORD || data->shape.kind == XR_LIR_TYPE_CLASS ||
        ((data->shape.kind == XR_LIR_TYPE_NULLABLE || data->shape.kind == XR_LIR_TYPE_TUPLE) && data->layout.managed);
}

XR_FUNC bool xr_lir_block_owner_type(const XrLirModule *m, XrLirType type) {
    return type == XR_LIR_STRING || type == XR_LIR_ARRAY || xr_lir_map_element(type) ||
        type == XR_LIR_SET_I64 || xr_lir_boxed_type(m, type);
}

XR_FUNC XrLirStatus xr_lir_callable_type(XrLirModule *m, const XrLirCallableType *signature,
    XrLirType *output) {
    if (!signature || signature->parameter_count > 65536 || (signature->parameter_count && !signature->parameters))
        return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
    if (signature->throws || signature->result == XR_LIR_MAP_STRING_STRING) return lir_fail(m, XR_LIR_UNSUPPORTED);
    for (uint32_t p = 0; p < signature->parameter_count; ++p)
        if (signature->parameters[p].type == XR_LIR_MAP_STRING_STRING) return lir_fail(m, XR_LIR_UNSUPPORTED);
    return lir_intern_type(m, (XrLirTypeView){XR_LIR_TYPE_CALLABLE,
        signature->result, signature->parameter_count}, signature, 0, 0, NULL, (LirNominalType){0}, NULL, output);
}

XR_FUNC XrLirStatus xr_lir_cell_type(XrLirModule *m, XrLirCellType cell, XrLirType *output) {
    return lir_intern_type(m, (XrLirTypeView){XR_LIR_TYPE_CELL, cell.payload, 0}, NULL, cell.array_element, 0, NULL, (LirNominalType){0}, NULL, output);
}

XR_FUNC XrLirStatus xr_lir_cell_type_view(const XrLirModule *m, XrLirType type, XrLirCellType *output) {
    XrLirTypeView shape;
    if (!output || xr_lir_type_view(m, type, &shape) != XR_LIR_OK || shape.kind != XR_LIR_TYPE_CELL)
        return XR_LIR_INVALID_ARGUMENT;
    *output = (XrLirCellType){shape.element, m->types[type - XR_LIR_TYPE_COUNT].cell_element};
    return XR_LIR_OK;
}
XR_FUNC bool xr_lir_cell_value_type(const XrLirModule *m, XrLirType type, XrLirCellType *output) {
    XrLirCellType cell;
    if (xr_lir_cell_type_view(m, type, &cell) != XR_LIR_OK || cell.array_element ||
        (cell.payload != XR_LIR_I64 && cell.payload != XR_LIR_MAP_STRING_I64)) return false;
    if (output) *output = cell;
    return true;
}

XR_FUNC bool xr_lir_inline_aggregate_type(const XrLirModule *m, XrLirType type) {
    while (m && type >= XR_LIR_TYPE_COUNT && type - XR_LIR_TYPE_COUNT < m->type_count) {
        XrLirTypeData data = m->types[type - XR_LIR_TYPE_COUNT];
        if (data.shape.kind == XR_LIR_TYPE_RECORD) return !data.class_payload_only;
        if (data.shape.kind == XR_LIR_TYPE_TUPLE) return true;
        if (data.shape.kind != XR_LIR_TYPE_NULLABLE || data.shape.element >= type) return false;
        if (data.layout.managed) return true;
        type = data.shape.element;
    }
    return false;
}

XR_FUNC XrLirStatus xr_lir_type_view(const XrLirModule *m, XrLirType type,
    XrLirTypeView *output) {
    if (!m || m->failure != XR_LIR_OK || !output) return XR_LIR_INVALID_ARGUMENT;
    if (type >= XR_LIR_TYPE_COUNT) {
        const XrLirTypeData *data = xr_lir_internal_constructed_data(m, type);
        if (!data) return XR_LIR_INVALID_ARGUMENT;
        *output = data->shape;
    } else {
        if (!xr_lir_internal_type_valid(m, type, true)) return XR_LIR_INVALID_ARGUMENT;
        *output = (XrLirTypeView){XR_LIR_TYPE_BUILTIN, XR_LIR_TYPE_INVALID, 0};
    }
    return XR_LIR_OK;
}

XR_FUNC XrLirStatus xr_lir_type_layout(const XrLirModule *m, XrLirType type,
    XrLirTypeLayout *output) {
    XrLirTypeView view;
    XrLirStatus status = xr_lir_type_view(m, type, &view);
    if (!output || status != XR_LIR_OK) return XR_LIR_INVALID_ARGUMENT;
    if (view.kind == XR_LIR_TYPE_CALLABLE || view.kind == XR_LIR_TYPE_TUPLE ||
        (view.kind == XR_LIR_TYPE_NULLABLE && !m->types[type - XR_LIR_TYPE_COUNT].layout.size) ||
        (view.kind == XR_LIR_TYPE_CELL && !xr_lir_cell_value_type(m,type,NULL))) return XR_LIR_UNSUPPORTED;
    XrLirTypeLayout layout = {0, 1, 1, false};
    if (view.kind != XR_LIR_TYPE_BUILTIN) {
        layout = m->types[type - XR_LIR_TYPE_COUNT].layout;
    } else {
        switch (type) {
        case XR_LIR_UNIT: layout.scalar_count = 0; break;
        case XR_LIR_I1: case XR_LIR_I8: case XR_LIR_U8: layout.size = 1; break;
        case XR_LIR_I16: case XR_LIR_U16: layout.size = 2; layout.alignment = 2; break;
        case XR_LIR_I32: case XR_LIR_U32: case XR_LIR_F32:
            layout.size = 4; layout.alignment = 4; break;
        case XR_LIR_I64: case XR_LIR_U64: case XR_LIR_F64: case XR_LIR_PTR:
            layout.size = 8; layout.alignment = 8; break;
        case XR_LIR_STRING: case XR_LIR_ARRAY: case XR_LIR_MAP_STRING_I64: case XR_LIR_MAP_STRING_STRING: case XR_LIR_SET_I64:
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
    uint64_t offset = 0;
    while (type >= XR_LIR_TYPE_COUNT) {
        XrLirTypeView shape = m->types[type - XR_LIR_TYPE_COUNT].shape;
        if (shape.kind == XR_LIR_TYPE_FIXED_ARRAY) {
            *output = (XrLirScalarPart){shape.element,
                offset + (uint64_t)ordinal * xr_lir_fixed_element_size(shape.element)};
            return XR_LIR_OK;
        }
        if (!ordinal) { *output = (XrLirScalarPart){XR_LIR_U8, offset}; return XR_LIR_OK; }
        --ordinal;
        XrLirTypeLayout payload;
        status = xr_lir_type_layout(m, shape.element, &payload);
        if (status != XR_LIR_OK) return status;
        offset += payload.alignment;
        type = shape.element;
    }
    *output = (XrLirScalarPart){type, offset};
    return XR_LIR_OK;
}

#include "xlir_callable_import.inc.c"

XR_FUNC XrLirStatus xr_lir_import_type(XrLirModule *target, const XrLirModule *source,
    XrLirType type, XrLirType *output) {
    XrLirStatus status = lir_writable(target);
    if (status != XR_LIR_OK) return status;
    XrLirTypeView view;
    if (!output || xr_lir_type_view(source, type, &view) != XR_LIR_OK)
        return lir_fail(target, XR_LIR_INVALID_ARGUMENT);
    if (type >= XR_LIR_TYPE_COUNT) return lir_import_dependencies(target, source, type, output);
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
        status = lir_intern_type(target, source->types[n].shape,
            source->types[n].shape.kind == XR_LIR_TYPE_CALLABLE ? &source->types[n].callable : NULL,
            source->types[n].cell_element, source->types[n].class_parent, source->types[n].fields,
            (LirNominalType){.identity=source->types[n].record.name ? &source->types[n].record : NULL,
                .allow_class_fields=source->types[n].class_payload_only},
            source->types[n].shape.kind==XR_LIR_TYPE_SUM ? source->types[n].variants : NULL, &imported);
        if (status != XR_LIR_OK) return status;
        XR_DCHECK(imported == XR_LIR_TYPE_COUNT + n, "a fresh receiving pool preserves canonical order");
        target->types[n].inline_array_ordinal = source->types[n].inline_array_ordinal;
    }
    target->inline_array_count = source->inline_array_count;
    return XR_LIR_OK;
}
