/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_value_layout.c - Metered natural inline layouts from the owned LIR type DAG
 *
 * KEY CONCEPT:
 *   The closed type pool supplies identities and dependencies. This cold
 *   producer alone computes field positions and flattened physical owner leaves.
 */
#include "xlir_value_layout.h"
#include "xlir_internal.h"
#include "xray_native_function.h"
#include "base/xchecks.h"
#include <stdalign.h>
#include <string.h>

enum { VALUE_ARRAY_KINDS = 4 };
typedef struct LayoutRow {
    XrLirValueLayoutKey key;
    XrtValuePlan *plan;
    uint32_t id;
    bool needed;
} LayoutRow;
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4200) /* C11 flexible array member */
#endif
struct XrLirValueLayouts {
    XrtBillingOwner *billing;
    size_t bytes, rows, count, constructed;
    LayoutRow **ordered;
    XrLirOwnershipTypeFact *types;
    uint32_t type_count;
    LayoutRow values[];
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
_Static_assert(sizeof(XrLirValueLayouts) % alignof(LayoutRow) == 0, "layout rows follow an aligned fixed prefix");
_Static_assert(sizeof(LayoutRow) % alignof(LayoutRow *) == 0 &&
    sizeof(LayoutRow *) % alignof(XrLirOwnershipTypeFact) == 0,
    "ordered rows and ownership facts remain aligned in the same billed block");

static XrLirValueLayoutStatus memory_status(XrtManagedStatus status) {
    switch (status) {
    case XRT_MANAGED_OK: return XR_LIR_VALUE_LAYOUT_OK;
    case XRT_MANAGED_OOM: return XR_LIR_VALUE_LAYOUT_OOM;
    case XRT_MANAGED_LIMIT: return XR_LIR_VALUE_LAYOUT_LIMIT;
    case XRT_MANAGED_CLOSED: return XR_LIR_VALUE_LAYOUT_CLOSED;
    case XRT_MANAGED_REFCOUNT: return XR_LIR_VALUE_LAYOUT_REFCOUNT;
    case XRT_MANAGED_OVERFLOW: return XR_LIR_VALUE_LAYOUT_OVERFLOW;
    default: return XR_LIR_VALUE_LAYOUT_ARGUMENT;
    }
}
static bool extent_add(size_t *size, size_t count, size_t stride) {
    if (stride && count > (SIZE_MAX - *size) / stride) return false;
    *size += count * stride;
    return true;
}
static bool align_size(size_t *size, size_t alignment) {
    if (!alignment || (alignment & (alignment - 1)) || *size > SIZE_MAX - (alignment - 1)) return false;
    *size = (*size + alignment - 1) & ~(alignment - 1);
    return true;
}
static XrLirType array_element(size_t family) {
    static const XrLirType elements[] = {XR_LIR_I64, XR_LIR_STRING, XR_LIR_U8, XR_LIR_U64};
    return elements[family];
}
static XrLirValueLayoutStatus key_index(const XrLirValueLayouts *set, XrLirValueLayoutKey key, size_t *index) {
    if (key.type == XR_LIR_ARRAY) {
        for (size_t i = 0; i < VALUE_ARRAY_KINDS; ++i)
            if (key.array_element == array_element(i)) { *index = XR_LIR_TYPE_COUNT + set->constructed + i; return XR_LIR_VALUE_LAYOUT_OK; }
        return XR_LIR_VALUE_LAYOUT_UNSUPPORTED;
    }
    if (key.array_element || !key.type || key.type >= XR_LIR_TYPE_COUNT + set->constructed)
        return XR_LIR_VALUE_LAYOUT_ARGUMENT;
    *index = key.type;
    return XR_LIR_VALUE_LAYOUT_OK;
}
static XrLirValueLayoutStatus need_type(XrLirValueLayouts *set, XrLirType type, XrLirType parent) {
    if (type >= parent) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
    size_t index = 0;
    XrLirValueLayoutStatus status = key_index(set, (XrLirValueLayoutKey){type, 0}, &index);
    if (status == XR_LIR_VALUE_LAYOUT_OK) set->values[index].needed = true;
    return status;
}
static XrLirValueLayoutStatus need_callable_type(XrLirValueLayouts *set,
    XrLirType type, XrLirType element, XrLirType parent) {
    if (type >= parent) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
    if (type != XR_LIR_ARRAY) return element ? XR_LIR_VALUE_LAYOUT_UNSUPPORTED : need_type(set, type, parent);
    size_t index = 0;
    XrLirValueLayoutStatus status = key_index(set, (XrLirValueLayoutKey){type, element}, &index);
    if (status == XR_LIR_VALUE_LAYOUT_OK) set->values[index].needed = true;
    return status;
}
static XrLirValueLayoutStatus mark_dependencies(const XrLirModule *module, XrLirValueLayouts *set) {
    for (size_t row = XR_LIR_TYPE_COUNT + set->constructed; row-- > XR_LIR_TYPE_COUNT;) {
        if (!set->values[row].needed) continue;
        XrLirType type = (XrLirType)row;
        XrLirTypeView shape;
        if (xr_lir_type_view(module, type, &shape) != XR_LIR_OK) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
        if (shape.kind == XR_LIR_TYPE_NULLABLE) {
            XrLirValueLayoutStatus status = need_type(set, shape.element, type);
            if (status != XR_LIR_VALUE_LAYOUT_OK) return status;
        } else if (shape.kind == XR_LIR_TYPE_RECORD) {
            const XrLirRecordField *fields = NULL; uint32_t count = 0;
            if (xr_lir_record_fields(module, type, &fields, &count) != XR_LIR_OK || count != shape.count || (count && !fields))
                return XR_LIR_VALUE_LAYOUT_ARGUMENT;
            for (uint32_t f = 0; f < count; ++f) {
                XrLirValueLayoutStatus status = need_type(set, fields[f].type, type);
                if (status != XR_LIR_VALUE_LAYOUT_OK) return status;
            }
        } else if (shape.kind == XR_LIR_TYPE_CALLABLE) {
            XrLirCallableType signature;
            if (xr_lir_callable_type_view(module, type, &signature) != XR_LIR_OK) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
            if (signature.throws) return XR_LIR_VALUE_LAYOUT_UNSUPPORTED;
            XrLirValueLayoutStatus status = need_callable_type(set, signature.result, signature.result_element, type);
            for (uint32_t p = 0; p < signature.parameter_count && status == XR_LIR_VALUE_LAYOUT_OK; ++p)
                status = need_callable_type(set, signature.parameters[p].type, signature.parameters[p].array_element, type);
            if (status != XR_LIR_VALUE_LAYOUT_OK) return status;
        } else if (shape.kind != XR_LIR_TYPE_FIXED_ARRAY && shape.kind != XR_LIR_TYPE_TUPLE)
            return XR_LIR_VALUE_LAYOUT_UNSUPPORTED;
    }
    return XR_LIR_VALUE_LAYOUT_OK;
}
static XrLirValueLayoutStatus scalar_spec(XrLirValueLayoutKey key, XrtValuePlanSpec *spec) {
    size_t size = 0, alignment = 1;
    XrtValueKind kind;
    switch (key.type) {
    case XR_LIR_UNIT: kind = XRT_VALUE_UNIT; break;
    case XR_LIR_I1: kind = XRT_VALUE_BOOL; size = alignment = 1; break;
    case XR_LIR_I8: kind = XRT_VALUE_I8; size = alignment = 1; break;
    case XR_LIR_U8: kind = XRT_VALUE_U8; size = alignment = 1; break;
    case XR_LIR_I16: kind = XRT_VALUE_I16; size = alignment = 2; break;
    case XR_LIR_U16: kind = XRT_VALUE_U16; size = alignment = 2; break;
    case XR_LIR_I32: kind = XRT_VALUE_I32; size = alignment = 4; break;
    case XR_LIR_U32: kind = XRT_VALUE_U32; size = alignment = 4; break;
    case XR_LIR_F32: kind = XRT_VALUE_F32; size = alignment = 4; break;
    case XR_LIR_I64: kind = XRT_VALUE_I64; size = alignment = 8; break;
    case XR_LIR_U64: kind = XRT_VALUE_U64; size = alignment = 8; break;
    case XR_LIR_F64: kind = XRT_VALUE_F64; size = alignment = 8; break;
    case XR_LIR_STRING: kind = XRT_VALUE_STRING; size = alignment = 8; break;
    case XR_LIR_MAP_STRING_I64: kind = XRT_VALUE_MAP_STRING_I64; size = alignment = 8; break;
    case XR_LIR_ARRAY:
        kind = key.array_element == XR_LIR_I64 ? XRT_VALUE_ARRAY_I64 : key.array_element == XR_LIR_STRING ?
            XRT_VALUE_ARRAY_STRING : key.array_element == XR_LIR_U8 ? XRT_VALUE_ARRAY_U8 : XRT_VALUE_ARRAY_U64;
        size = alignment = 8;
        break;
    default: return XR_LIR_VALUE_LAYOUT_UNSUPPORTED;
    }
    *spec = (XrtValuePlanSpec){.kind=kind, .size=size, .alignment=alignment};
    if (kind >= XRT_VALUE_STRING && kind <= XRT_VALUE_MAP_STRING_I64) spec->owned_count = 1;
    return XR_LIR_VALUE_LAYOUT_OK;
}
static const XrtValuePlan *dependency(const XrLirValueLayouts *set, XrLirType type) {
    return set->values[type].plan;
}
static const XrtValuePlan *callable_dependency(const XrLirValueLayouts *set, XrLirType type, XrLirType element) {
    size_t index = 0;
    XR_CHECK(key_index(set, (XrLirValueLayoutKey){type, element}, &index) == XR_LIR_VALUE_LAYOUT_OK && set->values[index].plan,
        "signature dependencies precede their receiving callable plan");
    return set->values[index].plan;
}
static XrLirValueLayoutStatus composite_spec(const XrLirModule *module, XrLirValueLayouts *set,
    XrLirType type, XrtValuePlanSpec *spec, size_t *scratch_bytes) {
    XrLirTypeView shape;
    if (xr_lir_type_view(module, type, &shape) != XR_LIR_OK) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
    *spec = (XrtValuePlanSpec){0};
    if (shape.kind == XR_LIR_TYPE_CALLABLE) {
        XrLirCallableType signature;
        if (xr_lir_callable_type_view(module, type, &signature) != XR_LIR_OK) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
        spec->kind = XRT_VALUE_FUNCTION; spec->size = sizeof(XrtFunction); spec->alignment = alignof(XrtFunction);
        spec->owned_count = 1;
        spec->signature = (XrtValueSignatureSpec){.parameter_count=signature.parameter_count,
            .result=callable_dependency(set, signature.result, signature.result_element),
            .flags=((signature.flags & XR_LIR_CALLABLE_NO_SUSPEND) ? XRT_VALUE_NO_SUSPEND : 0u) |
                ((signature.flags & XR_LIR_CALLABLE_C_ABI) ? XRT_VALUE_C_ABI : 0u), .throws=signature.throws};
        if (!extent_add(scratch_bytes, signature.parameter_count, sizeof(XrtValueParameterSpec)) ||
            !extent_add(scratch_bytes, 1, sizeof(size_t))) return XR_LIR_VALUE_LAYOUT_OVERFLOW;
        return XR_LIR_VALUE_LAYOUT_OK;
    }
    if (shape.kind == XR_LIR_TYPE_FIXED_ARRAY || shape.kind == XR_LIR_TYPE_TUPLE) {
        bool fixed = shape.kind == XR_LIR_TYPE_FIXED_ARRAY;
        XrLirType element = fixed ? XR_LIR_U8 : XR_LIR_I64;
        if (!shape.count || shape.element != element) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
        spec->kind = fixed ? XRT_VALUE_FIXED_U8 : XRT_VALUE_TUPLE_I64;
        spec->count = shape.count; spec->alignment = fixed ? 1 : 8;
        if (!extent_add(&spec->size, shape.count, spec->alignment)) return XR_LIR_VALUE_LAYOUT_OVERFLOW;
        return XR_LIR_VALUE_LAYOUT_OK;
    }
    if (shape.kind == XR_LIR_TYPE_NULLABLE) {
        XrtValuePlanSpec child;
        const XrtValuePlan *plan = dependency(set, shape.element);
        if (xrt_value_plan_view(plan, &child) != XRT_MANAGED_OK) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
        spec->kind = XRT_VALUE_NULLABLE; spec->alignment = child.alignment;
        spec->nullable = (XrtValueNullableSpec){0, child.alignment, plan};
        spec->size = child.alignment; spec->owned_count = child.owned_count;
        if (!extent_add(&spec->size, child.size, 1) || !align_size(&spec->size, child.alignment) ||
            !extent_add(scratch_bytes, child.owned_count, sizeof(size_t))) return XR_LIR_VALUE_LAYOUT_OVERFLOW;
        return XR_LIR_VALUE_LAYOUT_OK;
    }
    if (shape.kind != XR_LIR_TYPE_RECORD) return XR_LIR_VALUE_LAYOUT_UNSUPPORTED;
    const XrLirRecordField *fields = NULL; uint32_t count = 0;
    if (xr_lir_record_fields(module, type, &fields, &count) != XR_LIR_OK) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
    spec->kind = XRT_VALUE_RECORD; spec->alignment = 1; spec->field_count = count;
    if (!count) { spec->size = 1; return XR_LIR_VALUE_LAYOUT_OK; }
    for (uint32_t f = 0; f < count; ++f) {
        XrtValuePlanSpec child;
        if (xrt_value_plan_view(dependency(set, fields[f].type), &child) != XRT_MANAGED_OK || !child.size)
            return XR_LIR_VALUE_LAYOUT_ARGUMENT;
        if (child.alignment > spec->alignment) spec->alignment = child.alignment;
        if (!align_size(&spec->size, child.alignment) || !extent_add(&spec->size, child.size, 1) ||
            !extent_add(&spec->owned_count, child.owned_count, 1)) return XR_LIR_VALUE_LAYOUT_OVERFLOW;
    }
    if (!align_size(&spec->size, spec->alignment) || !extent_add(scratch_bytes, count, sizeof(XrtValueSlotSpec)) ||
        !extent_add(scratch_bytes, spec->owned_count, sizeof(size_t))) return XR_LIR_VALUE_LAYOUT_OVERFLOW;
    return XR_LIR_VALUE_LAYOUT_OK;
}
static void fill_composite(const XrLirModule *module, const XrLirValueLayouts *set, XrLirType type,
    XrtValuePlanSpec *spec, void *scratch) {
    size_t *owned;
    if (spec->kind == XRT_VALUE_FUNCTION) {
        XrLirCallableType signature;
        XR_CHECK(xr_lir_callable_type_view(module, type, &signature) == XR_LIR_OK,
            "finished callable contracts stay borrowed during projection");
        XrtValueParameterSpec *parameters = scratch;
        owned = (size_t *)(parameters + signature.parameter_count);
        *owned = offsetof(XrtFunction, owner); spec->owned_offsets = owned;
        spec->signature.parameters = signature.parameter_count ? parameters : NULL;
        for (uint32_t p = 0; p < signature.parameter_count; ++p) {
            XrLirCallableParameter parameter = signature.parameters[p];
            parameters[p] = (XrtValueParameterSpec){.type=callable_dependency(set, parameter.type, parameter.array_element),
                .mode=parameter.mode, .escaping=parameter.escaping};
        }
        return;
    }
    if (spec->kind == XRT_VALUE_NULLABLE) {
        XrtValuePlanSpec child;
        XR_CHECK(xrt_value_plan_view(spec->nullable.element, &child) == XRT_MANAGED_OK, "nullable dependency is independently owned");
        owned = scratch; spec->owned_offsets = owned;
        for (size_t i = 0; i < child.owned_count; ++i) owned[i] = spec->nullable.payload_offset + child.owned_offsets[i];
        return;
    }
    if (spec->kind != XRT_VALUE_RECORD) return;
    const XrLirRecordField *fields = NULL; uint32_t count = 0;
    XR_CHECK(xr_lir_record_fields(module, type, &fields, &count) == XR_LIR_OK, "finished record fields stay borrowed during projection");
    if (!count) return;
    XrtValueSlotSpec *slots = scratch;
    owned = (size_t *)(slots + count);
    spec->fields = slots; spec->owned_offsets = spec->owned_count ? owned : NULL;
    size_t offset = 0, leaves = 0;
    for (uint32_t f = 0; f < count; ++f) {
        const XrtValuePlan *plan = dependency(set, fields[f].type);
        XrtValuePlanSpec child;
        XR_CHECK(xrt_value_plan_view(plan, &child) == XRT_MANAGED_OK && align_size(&offset, child.alignment),
            "record offsets were checked before scratch allocation");
        slots[f] = (XrtValueSlotSpec){plan, offset, fields[f].name, fields[f].name_length};
        for (size_t i = 0; i < child.owned_count; ++i) owned[leaves++] = offset + child.owned_offsets[i];
        offset += child.size;
    }
    XR_CHECK(leaves == spec->owned_count, "every transitive owner leaf has one physical offset");
}
static XrLirValueLayoutStatus build_row(const XrLirModule *module, XrLirValueLayouts *set, size_t row) {
    if (set->count >= UINT32_MAX) return XR_LIR_VALUE_LAYOUT_OVERFLOW;
    XrtValuePlanSpec spec = {0};
    size_t scratch_bytes = 0, zero = 0;
    XrLirValueLayoutStatus status = row < XR_LIR_TYPE_COUNT || row >= XR_LIR_TYPE_COUNT + set->constructed ?
        scalar_spec(set->values[row].key, &spec) : composite_spec(module, set, (XrLirType)row, &spec, &scratch_bytes);
    if (status != XR_LIR_VALUE_LAYOUT_OK) return status;
    void *scratch = NULL;
    if (scratch_bytes) {
        status = memory_status(xrt_managed_metadata_allocate(set->billing, scratch_bytes, &scratch));
        if (status != XR_LIR_VALUE_LAYOUT_OK) return status;
    }
    if (row >= XR_LIR_TYPE_COUNT && row < XR_LIR_TYPE_COUNT + set->constructed)
        fill_composite(module, set, (XrLirType)row, &spec, scratch);
    else if (spec.owned_count) spec.owned_offsets = &zero;
    status = memory_status(xrt_value_plan_new(set->billing, &spec, &set->values[row].plan));
    if (scratch) xrt_managed_metadata_free(set->billing, scratch, scratch_bytes);
    if (status == XR_LIR_VALUE_LAYOUT_OK) {
        set->values[row].id = (uint32_t)set->count + 1;
        set->ordered[set->count++] = &set->values[row];
    }
    return status;
}
XR_FUNCDEF void xr_lir_value_layouts_destroy(XrLirValueLayouts *set) {
    if (!set) return;
    XrtBillingOwner *billing = set->billing;
    for (size_t i = set->rows; i; --i) xrt_value_plan_drop(set->values[i - 1].plan);
    xrt_managed_metadata_free(billing, set, set->bytes);
    xrt_billing_owner_drop(billing);
}
XR_FUNCDEF XrLirValueLayoutStatus xr_lir_value_layouts_create(const XrLirModule *module,
    const XrLirValueLayoutKey *roots, size_t count, XrtBillingOwner *billing, XrLirValueLayouts **output) {
    if (!module || !module->finished || module->failure != XR_LIR_OK || !billing || !output || *output || (count && !roots))
        return XR_LIR_VALUE_LAYOUT_ARGUMENT;
    size_t rows = XR_LIR_TYPE_COUNT + (size_t)module->type_count, bytes = sizeof(XrLirValueLayouts);
    if (!extent_add(&rows, VALUE_ARRAY_KINDS, 1) || !extent_add(&bytes, rows, sizeof(LayoutRow)) ||
        !extent_add(&bytes, rows, sizeof(LayoutRow *)) ||
        !extent_add(&bytes, module->type_count, sizeof(XrLirOwnershipTypeFact)))
        return XR_LIR_VALUE_LAYOUT_OVERFLOW;
    if (!xrt_billing_owner_retain(billing)) return XR_LIR_VALUE_LAYOUT_REFCOUNT;
    void *memory = NULL;
    XrLirValueLayoutStatus status = memory_status(xrt_managed_metadata_allocate(billing, bytes, &memory));
    if (status != XR_LIR_VALUE_LAYOUT_OK) { xrt_billing_owner_drop(billing); return status; }
    XrLirValueLayouts *set = memory;
    *set = (XrLirValueLayouts){.billing=billing, .bytes=bytes, .rows=rows, .constructed=module->type_count};
    set->ordered = (LayoutRow **)(set->values + rows);
    set->types = (XrLirOwnershipTypeFact *)(set->ordered + rows);
    for (size_t i = 0; i < rows; ++i) set->values[i] = (LayoutRow){.key={(XrLirType)i, 0}};
    for (size_t i = 0; i < VALUE_ARRAY_KINDS; ++i)
        set->values[rows - VALUE_ARRAY_KINDS + i].key = (XrLirValueLayoutKey){XR_LIR_ARRAY, array_element(i)};
    for (size_t i = 0; i < count && status == XR_LIR_VALUE_LAYOUT_OK; ++i) {
        size_t index = 0;
        status = key_index(set, roots[i], &index);
        if (status == XR_LIR_VALUE_LAYOUT_OK) set->values[index].needed = true;
    }
    if (status == XR_LIR_VALUE_LAYOUT_OK) status = mark_dependencies(module, set);
    // Array descriptors have no inline type dependencies and precede aggregates.
    for (size_t i = rows - VALUE_ARRAY_KINDS; i < rows && status == XR_LIR_VALUE_LAYOUT_OK; ++i)
        if (set->values[i].needed) status = build_row(module, set, i);
    for (size_t i = 1; i < rows - VALUE_ARRAY_KINDS && status == XR_LIR_VALUE_LAYOUT_OK; ++i)
        if (set->values[i].needed) status = build_row(module, set, i);
    for (size_t i = XR_LIR_TYPE_COUNT; i < rows - VALUE_ARRAY_KINDS && status == XR_LIR_VALUE_LAYOUT_OK; ++i) {
        if (!set->values[i].plan) continue;
        XrtValuePlanSpec spec;
        status = memory_status(xrt_value_plan_view(set->values[i].plan, &spec));
        if (status == XR_LIR_VALUE_LAYOUT_OK && (spec.kind == XRT_VALUE_RECORD || spec.kind == XRT_VALUE_NULLABLE)) {
            XR_CHECK(set->type_count < set->constructed, "each ownership fact belongs to one generated constructed plan");
            set->types[set->type_count++] = (XrLirOwnershipTypeFact){(XrLirType)i, spec.owned_count != 0};
        }
    }
    if (status != XR_LIR_VALUE_LAYOUT_OK) { xr_lir_value_layouts_destroy(set); return status; }
    *output = set;
    return XR_LIR_VALUE_LAYOUT_OK;
}
XR_FUNCDEF size_t xr_lir_value_layouts_count(const XrLirValueLayouts *set) { return set ? set->count : 0; }
XR_FUNCDEF XrLirValueLayoutStatus xr_lir_value_layouts_ownership(const XrLirValueLayouts *set,
    const XrLirOwnershipTypeFact **types, uint32_t *count) {
    if (!set || !types || !count) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
    *types = set->type_count ? set->types : NULL;
    *count = set->type_count;
    return XR_LIR_VALUE_LAYOUT_OK;
}
XR_FUNCDEF XrLirValueLayoutStatus xr_lir_value_layouts_get(const XrLirValueLayouts *set,
    XrLirValueLayoutKey key, const XrtValuePlan **output) {
    uint32_t id;
    return xr_lir_value_layouts_lookup(set, key, output, &id);
}
XR_FUNCDEF XrLirValueLayoutStatus xr_lir_value_layouts_lookup(const XrLirValueLayouts *set,
    XrLirValueLayoutKey key, const XrtValuePlan **output, uint32_t *id) {
    if (!set || !output || !id) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
    size_t index = 0;
    XrLirValueLayoutStatus status = key_index(set, key, &index);
    if (status != XR_LIR_VALUE_LAYOUT_OK) return status;
    if (!set->values[index].plan) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
    *output = set->values[index].plan;
    *id = set->values[index].id;
    return XR_LIR_VALUE_LAYOUT_OK;
}
XR_FUNCDEF XrLirValueLayoutStatus xr_lir_value_layouts_at(const XrLirValueLayouts *set, size_t ordinal,
    XrLirValueLayoutKey *key, const XrtValuePlan **output) {
    if (!set || !key || !output || ordinal >= set->count) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
    const LayoutRow *row = set->ordered[ordinal];
    XR_CHECK(row && row->plan, "a sealed ordinal names its independently owned receiving plan");
    *key = row->key; *output = row->plan;
    return XR_LIR_VALUE_LAYOUT_OK;
}
