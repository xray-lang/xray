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
#include "xray_native_array_slice.h"
#include "base/xchecks.h"
#include <stdalign.h>
#include <string.h>

typedef struct LayoutRow {
    XrLirValueLayoutKey key;
    uint32_t inline_array_ordinal;
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
    size_t bytes, rows, count, constructed, array_rows, functions;
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
/* A record declaration may raise its own alignment; every other row is natural. */
static uint32_t declared_record_alignment(const XrLirModule *module, XrLirType type) {
    return module && type >= XR_LIR_TYPE_COUNT && type - XR_LIR_TYPE_COUNT < module->type_count ?
        xr_lir_record_alignment(module->types[type - XR_LIR_TYPE_COUNT].record.flags) : 0;
}
static XrLirValueLayoutStatus key_index(const XrLirValueLayouts *set, XrLirValueLayoutKey key, size_t *index) {
    if (key.environment_function) {
        if (key.array_element || key.environment_function > set->functions) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
        *index = XR_LIR_TYPE_COUNT + set->constructed + set->array_rows + key.environment_function - 1;
        return key.type && set->values[*index].key.type == key.type ? XR_LIR_VALUE_LAYOUT_OK : XR_LIR_VALUE_LAYOUT_ARGUMENT;
    }
    if (key.type == XR_LIR_ARRAY) {
        size_t arrays = XR_LIR_TYPE_COUNT + set->constructed;
        uint32_t family = xr_lir_internal_array_family(key.array_element);
        if (family < XR_LIR_ARRAY_FAMILY_COUNT) {
            *index = arrays + family;
            return XR_LIR_VALUE_LAYOUT_OK;
        }
        if (key.array_element >= XR_LIR_TYPE_COUNT && key.array_element < arrays) {
            uint32_t ordinal = set->values[key.array_element].inline_array_ordinal;
            if (ordinal) {
                *index = arrays + XR_LIR_ARRAY_FAMILY_COUNT + ordinal - 1;
                return XR_LIR_VALUE_LAYOUT_OK;
            }
        }
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
    XrLirValueLayoutStatus status = key_index(set, (XrLirValueLayoutKey){type, 0, 0}, &index);
    if (status == XR_LIR_VALUE_LAYOUT_OK) set->values[index].needed = true;
    return status;
}
static XrLirValueLayoutStatus need_shaped_type(XrLirValueLayouts *set,
    XrLirType type, XrLirType element, XrLirType parent) {
    if (type >= parent) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
    if (type != XR_LIR_ARRAY) return element ? XR_LIR_VALUE_LAYOUT_UNSUPPORTED : need_type(set, type, parent);
    if (element >= parent) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
    size_t index = 0;
    XrLirValueLayoutStatus status = key_index(set, (XrLirValueLayoutKey){type, element, 0}, &index);
    if (status == XR_LIR_VALUE_LAYOUT_OK) {
        set->values[index].needed = true;
        if (element >= XR_LIR_TYPE_COUNT) set->values[element].needed = true;
    }
    return status;
}
static XrLirValueLayoutStatus mark_dependencies(const XrLirModule *module, XrLirValueLayouts *set) {
    size_t first_inline = XR_LIR_TYPE_COUNT + set->constructed + XR_LIR_ARRAY_FAMILY_COUNT;
    size_t environments = XR_LIR_TYPE_COUNT + set->constructed + set->array_rows;
    for (size_t row = first_inline; row < environments; ++row)
        if (set->values[row].needed) set->values[set->values[row].key.array_element].needed = true;
    for (size_t row = environments; row < set->rows; ++row) {
        if (!set->values[row].needed) continue;
        const XrLirFunctionData *target = &module->functions[row - environments];
        bool assertion = target->kind == XR_LIR_FUNCTION_EXTERNAL && target->extern_kind == XR_LIR_EXTERN_ASSERT_CONDITION;
        if (!target->closure.callable || (target->kind != XR_LIR_FUNCTION_DEFINITION && !assertion) || target->closure.state_capture)
            return XR_LIR_VALUE_LAYOUT_UNSUPPORTED;
        XrLirType limit = (XrLirType)(XR_LIR_TYPE_COUNT + set->constructed);
        XrLirValueLayoutStatus status = need_type(set,target->closure.callable,limit);
        for (uint32_t p = 0; p < target->closure.capture_count && status == XR_LIR_VALUE_LAYOUT_OK; ++p)
            status = need_type(set,target->parameter_types[p],limit);
        if (status != XR_LIR_VALUE_LAYOUT_OK) return status;
    }
    for (size_t row = XR_LIR_TYPE_COUNT + set->constructed; row-- > XR_LIR_TYPE_COUNT;) {
        if (!set->values[row].needed) continue;
        XrLirType type = (XrLirType)row;
        XrLirTypeView shape;
        if (xr_lir_type_view(module, type, &shape) != XR_LIR_OK) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
        if (shape.kind == XR_LIR_TYPE_NULLABLE) {
            XrLirType element = xr_lir_nullable_array_element(module, type);
            XrLirValueLayoutStatus status = need_shaped_type(set, shape.element, element, type);
            if (status != XR_LIR_VALUE_LAYOUT_OK) return status;
        } else if (shape.kind == XR_LIR_TYPE_CLASS) {
            XrLirValueLayoutStatus status = need_type(set, shape.element, type);
            XrLirType parent = xr_lir_class_parent(module, type);
            if (status == XR_LIR_VALUE_LAYOUT_OK && parent) status = need_type(set, parent, type);
            if (status != XR_LIR_VALUE_LAYOUT_OK) return status;
        } else if (shape.kind == XR_LIR_TYPE_CELL) {
            XrLirCellType cell;
            if (!xr_lir_cell_value_type(module, type, &cell)) return XR_LIR_VALUE_LAYOUT_UNSUPPORTED;
            XrLirValueLayoutStatus status = need_type(set, cell.payload, type);
            if (status != XR_LIR_VALUE_LAYOUT_OK) return status;
        } else if (shape.kind == XR_LIR_TYPE_RECORD || shape.kind == XR_LIR_TYPE_TUPLE) {
            const XrLirRecordField *fields = NULL; uint32_t count = 0;
            if (xr_lir_aggregate_fields(module, type, &fields, &count) != XR_LIR_OK || count != shape.count || (count && !fields))
                return XR_LIR_VALUE_LAYOUT_ARGUMENT;
            for (uint32_t f = 0; f < count; ++f) {
                XrLirValueLayoutStatus status = need_shaped_type(set, fields[f].type, fields[f].array_element, type);
                if (status != XR_LIR_VALUE_LAYOUT_OK) return status;
            }
        } else if (shape.kind == XR_LIR_TYPE_SUM) {
            const XrLirRecordField *fields = NULL; uint32_t field_count = 0;
            const XrLirSumVariant *variants = NULL; uint32_t variant_count = 0;
            if (!xr_lir_sum_variants(module, type, &fields, &field_count, &variants, &variant_count) ||
                (field_count && !fields) || !variant_count) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
            for (uint32_t f = 0; f < field_count; ++f) {
                XrLirValueLayoutStatus status = need_type(set, fields[f].type, type);
                if (status != XR_LIR_VALUE_LAYOUT_OK) return status;
            }
        } else if (shape.kind == XR_LIR_TYPE_CALLABLE) {
            XrLirCallableType signature;
            if (xr_lir_callable_type_view(module, type, &signature) != XR_LIR_OK) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
            if (signature.throws) return XR_LIR_VALUE_LAYOUT_UNSUPPORTED;
            XrLirValueLayoutStatus status = need_shaped_type(set, signature.result, signature.result_element, type);
            for (uint32_t p = 0; p < signature.parameter_count && status == XR_LIR_VALUE_LAYOUT_OK; ++p)
                status = need_shaped_type(set, signature.parameters[p].type, signature.parameters[p].array_element, type);
            if (status != XR_LIR_VALUE_LAYOUT_OK) return status;
        } else if (shape.kind == XR_LIR_TYPE_FIXED_ARRAY) {
            /* The element is a plain scalar row, so the fixed plan's size and
             * alignment are read from it instead of being restated here. */
            XrLirValueLayoutStatus status = need_type(set, shape.element, type);
            if (status != XR_LIR_VALUE_LAYOUT_OK) return status;
        } else if (shape.kind != XR_LIR_TYPE_SLICE)
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
    case XR_LIR_MAP_STRING_STRING: kind = XRT_VALUE_MAP_STRING_STRING; size = alignment = 8; break;
    case XR_LIR_SET_I64: kind = XRT_VALUE_SET_I64; size = alignment = 8; break;
    case XR_LIR_ARRAY:
        kind = key.array_element == XR_LIR_I64 ? XRT_VALUE_ARRAY_I64 : key.array_element == XR_LIR_STRING ?
            XRT_VALUE_ARRAY_STRING : key.array_element == XR_LIR_U8 ? XRT_VALUE_ARRAY_U8 :
            key.array_element == XR_LIR_U64 ? XRT_VALUE_ARRAY_U64 :
            key.array_element == XR_LIR_F64 ? XRT_VALUE_ARRAY_F64 : XRT_VALUE_ARRAY_INLINE;
        size = alignment = 8;
        break;
    default: return XR_LIR_VALUE_LAYOUT_UNSUPPORTED;
    }
    *spec = (XrtValuePlanSpec){.kind=kind, .size=size, .alignment=alignment};
    if (kind == XRT_VALUE_STRING || xrt_value_kind_is_array(kind) || kind == XRT_VALUE_MAP_STRING_I64 ||
        kind == XRT_VALUE_MAP_STRING_STRING || kind == XRT_VALUE_SET_I64)
        spec->owned_count = 1;
    return XR_LIR_VALUE_LAYOUT_OK;
}
static const XrtValuePlan *dependency(const XrLirValueLayouts *set, XrLirType type) {
    return set->values[type].plan;
}
static const XrtValuePlan *shaped_dependency(const XrLirValueLayouts *set, XrLirType type, XrLirType element) {
    size_t index = 0;
    XR_CHECK(key_index(set, (XrLirValueLayoutKey){type, element, 0}, &index) == XR_LIR_VALUE_LAYOUT_OK && set->values[index].plan,
        "shaped dependencies precede their containing value plan");
    return set->values[index].plan;
}
static XrLirValueLayoutStatus environment_spec(const XrLirModule *module, const XrLirValueLayouts *set,
    uint32_t function, XrtValuePlanSpec *spec, size_t *scratch_bytes) {
    const XrLirFunctionData *target = &module->functions[function - 1];
    *spec = (XrtValuePlanSpec){.kind=XRT_VALUE_ENVIRONMENT,.alignment=1,.capture_count=target->closure.capture_count,
        .entry={function,dependency(set,target->closure.callable)}};
    for (uint32_t p = 0; p < target->closure.capture_count; ++p) {
        XrtValuePlanSpec child;
        if (xrt_value_plan_view(dependency(set,target->parameter_types[p]),&child) != XRT_MANAGED_OK)
            return XR_LIR_VALUE_LAYOUT_ARGUMENT;
        if (child.kind != XRT_VALUE_BOOL && child.kind != XRT_VALUE_I64 && child.kind != XRT_VALUE_U8 &&
            child.kind != XRT_VALUE_U64 && child.kind != XRT_VALUE_F64 && child.kind != XRT_VALUE_FUNCTION &&
            child.kind != XRT_VALUE_MAP_STRING_I64 && child.kind != XRT_VALUE_CELL) return XR_LIR_VALUE_LAYOUT_UNSUPPORTED;
        if (child.alignment > spec->alignment) spec->alignment = child.alignment;
        if (!align_size(&spec->size,child.alignment) || !extent_add(&spec->size,child.size,1) ||
            !extent_add(&spec->owned_count,child.owned_count,1)) return XR_LIR_VALUE_LAYOUT_OVERFLOW;
    }
    if (!align_size(&spec->size,spec->alignment) ||
        !extent_add(scratch_bytes,spec->capture_count,sizeof(XrtValueSlotSpec)) ||
        !extent_add(scratch_bytes,spec->owned_count,sizeof(size_t))) return XR_LIR_VALUE_LAYOUT_OVERFLOW;
    return XR_LIR_VALUE_LAYOUT_OK;
}
static void fill_environment(const XrLirModule *module, const XrLirValueLayouts *set,
    uint32_t function, XrtValuePlanSpec *spec, void *scratch) {
    if (!spec->capture_count) return;
    const XrLirFunctionData *target = &module->functions[function - 1];
    XrtValueSlotSpec *captures = scratch; size_t *owned = (size_t *)(captures + spec->capture_count);
    spec->captures = captures; spec->owned_offsets = spec->owned_count ? owned : NULL;
    size_t offset = 0, leaves = 0;
    for (uint32_t p = 0; p < target->closure.capture_count; ++p) {
        const XrtValuePlan *plan = dependency(set,target->parameter_types[p]); XrtValuePlanSpec child;
        XR_CHECK(xrt_value_plan_view(plan,&child) == XRT_MANAGED_OK && align_size(&offset,child.alignment),
            "environment offsets were checked before receiving scratch allocation");
        captures[p] = (XrtValueSlotSpec){.type = plan, .offset = offset};
        for (size_t n = 0; n < child.owned_count; ++n) owned[leaves++] = offset + child.owned_offsets[n];
        offset += child.size;
    }
    XR_CHECK(leaves == spec->owned_count,"every environment owning leaf has one physical offset");
}
static XrLirValueLayoutStatus composite_spec(const XrLirModule *module, XrLirValueLayouts *set,
    XrLirType type, XrtValuePlanSpec *spec, size_t *scratch_bytes) {
    XrLirTypeView shape;
    if (xr_lir_type_view(module, type, &shape) != XR_LIR_OK) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
    *spec = (XrtValuePlanSpec){0};
    if (shape.kind == XR_LIR_TYPE_CLASS) {
        XrLirType parent = xr_lir_class_parent(module, type);
        *spec = (XrtValuePlanSpec){.kind=XRT_VALUE_CLASS,.size=sizeof(void *),.alignment=alignof(void *),
            .owned_count=1,.element=dependency(set,shape.element),.class_parent=parent ? dependency(set,parent) : NULL,
            .class_storage=xr_lir_class_storage(module,type)};
        return extent_add(scratch_bytes,1,sizeof(size_t)) ? XR_LIR_VALUE_LAYOUT_OK : XR_LIR_VALUE_LAYOUT_OVERFLOW;
    }
    if (shape.kind == XR_LIR_TYPE_CELL) {
        XrLirCellType cell;
        if (!xr_lir_cell_value_type(module, type, &cell)) return XR_LIR_VALUE_LAYOUT_UNSUPPORTED;
        *spec = (XrtValuePlanSpec){.kind=XRT_VALUE_CELL,.size=sizeof(void *),.alignment=alignof(void *),
            .owned_count=1,.element=dependency(set,cell.payload)};
        return extent_add(scratch_bytes,1,sizeof(size_t)) ? XR_LIR_VALUE_LAYOUT_OK : XR_LIR_VALUE_LAYOUT_OVERFLOW;
    }
    if (shape.kind == XR_LIR_TYPE_SLICE) {
        if (shape.element != XR_LIR_I64 && shape.element != XR_LIR_U8) return XR_LIR_VALUE_LAYOUT_UNSUPPORTED;
        spec->kind = shape.element == XR_LIR_I64 ? XRT_VALUE_SLICE_I64 : XRT_VALUE_SLICE_U8;
        spec->size = sizeof(XrtReadonlyArraySlice); spec->alignment = alignof(XrtReadonlyArraySlice);
        return XR_LIR_VALUE_LAYOUT_OK;
    }
    if (shape.kind == XR_LIR_TYPE_CALLABLE) {
        XrLirCallableType signature;
        if (xr_lir_callable_type_view(module, type, &signature) != XR_LIR_OK) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
        spec->kind = XRT_VALUE_FUNCTION; spec->size = sizeof(XrtFunction); spec->alignment = alignof(XrtFunction);
        spec->owned_count = 1;
        spec->signature = (XrtValueSignatureSpec){.parameter_count=signature.parameter_count,
            .result=shaped_dependency(set, signature.result, signature.result_element),
            .flags=((signature.flags & XR_LIR_CALLABLE_NO_SUSPEND) ? XRT_VALUE_NO_SUSPEND : 0u) |
                ((signature.flags & XR_LIR_CALLABLE_C_ABI) ? XRT_VALUE_C_ABI : 0u), .throws=signature.throws};
        if (!extent_add(scratch_bytes, signature.parameter_count, sizeof(XrtValueParameterSpec)) ||
            !extent_add(scratch_bytes, 1, sizeof(size_t))) return XR_LIR_VALUE_LAYOUT_OVERFLOW;
        return XR_LIR_VALUE_LAYOUT_OK;
    }
    if (shape.kind == XR_LIR_TYPE_FIXED_ARRAY) {
        if (!shape.count || !xr_lir_fixed_element_size(shape.element)) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
        XrtValuePlanSpec element;
        const XrtValuePlan *plan = dependency(set, shape.element);
        if (xrt_value_plan_view(plan, &element) != XRT_MANAGED_OK) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
        spec->kind = XRT_VALUE_FIXED_INT;
        spec->count = shape.count;
        spec->element = plan;
        spec->alignment = element.alignment;
        spec->size = (size_t)shape.count * element.size;
        return XR_LIR_VALUE_LAYOUT_OK;
    }
    if (shape.kind == XR_LIR_TYPE_NULLABLE) {
        XrtValuePlanSpec child;
        const XrtValuePlan *plan = shaped_dependency(set, shape.element, xr_lir_nullable_array_element(module, type));
        if (xrt_value_plan_view(plan, &child) != XRT_MANAGED_OK) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
        spec->kind = XRT_VALUE_NULLABLE; spec->alignment = child.alignment;
        spec->nullable = (XrtValueNullableSpec){0, child.alignment, plan};
        spec->size = child.alignment; spec->owned_count = child.owned_count;
        if (!extent_add(&spec->size, child.size, 1) || !align_size(&spec->size, child.alignment) ||
            !extent_add(scratch_bytes, child.owned_count, sizeof(size_t))) return XR_LIR_VALUE_LAYOUT_OVERFLOW;
        return XR_LIR_VALUE_LAYOUT_OK;
    }
    if (shape.kind == XR_LIR_TYPE_SUM) {
        const XrLirRecordField *fields = NULL; uint32_t field_count = 0;
        const XrLirSumVariant *variants = NULL; uint32_t variant_count = 0;
        if (!xr_lir_sum_variants(module, type, &fields, &field_count, &variants, &variant_count) ||
            (field_count && !fields) || !variant_count) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
        uint32_t tag = xr_lir_sum_tag_size(variant_count);
        size_t alignment = tag ? tag : 1;
        for (uint32_t f = 0; f < field_count; ++f) {
            XrtValuePlanSpec child;
            if (xrt_value_plan_view(dependency(set, fields[f].type), &child) != XRT_MANAGED_OK || !child.size ||
                child.owned_count) return XR_LIR_VALUE_LAYOUT_UNSUPPORTED;
            if (child.alignment > alignment) alignment = child.alignment;
        }
        size_t payload = (tag + alignment - 1) & ~(alignment - 1), maximum = 0;
        for (uint32_t v = 0; v < variant_count; ++v) {
            size_t end = payload;
            for (uint32_t f = 0; f < variants[v].field_count; ++f) {
                XrtValuePlanSpec child;
                if (xrt_value_plan_view(dependency(set, fields[variants[v].field_begin + f].type), &child) != XRT_MANAGED_OK)
                    return XR_LIR_VALUE_LAYOUT_ARGUMENT;
                if (end > SIZE_MAX - (child.alignment - 1)) return XR_LIR_VALUE_LAYOUT_OVERFLOW;
                size_t offset = (end + child.alignment - 1) & ~(child.alignment - 1);
                if (child.size > SIZE_MAX - offset) return XR_LIR_VALUE_LAYOUT_OVERFLOW;
                end = offset + child.size;
            }
            if (end > maximum) maximum = end;
        }
        if (maximum > SIZE_MAX - (alignment - 1)) return XR_LIR_VALUE_LAYOUT_OVERFLOW;
        spec->kind = XRT_VALUE_SUM;
        spec->size = xrt_value_sum_size(maximum, alignment);
        spec->alignment = alignment;
        spec->field_count = field_count;
        spec->sum = (XrtValueSumSpec){0, tag, payload, NULL, variant_count};
        /* The sum's plan variant rows are the copied LIR rows; fill_composite
         * replaces them with module-owned copies. */
        if (!extent_add(scratch_bytes, field_count, sizeof(XrtValueSlotSpec)) ||
            !extent_add(scratch_bytes, variant_count, sizeof(XrtValueSumVariantSpec)))
            return XR_LIR_VALUE_LAYOUT_OVERFLOW;
        return XR_LIR_VALUE_LAYOUT_OK;
    }
    if (shape.kind != XR_LIR_TYPE_RECORD && shape.kind != XR_LIR_TYPE_TUPLE) return XR_LIR_VALUE_LAYOUT_UNSUPPORTED;
    const XrLirRecordField *fields = NULL; uint32_t count = 0;
    if (xr_lir_aggregate_fields(module, type, &fields, &count) != XR_LIR_OK) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
    spec->kind = shape.kind == XR_LIR_TYPE_TUPLE ? XRT_VALUE_TUPLE : XRT_VALUE_RECORD;
    spec->alignment = 1; spec->field_count = count;
    if (!count) {
        /* Portable C has no empty structure: one private byte carries the
         * record, and a declared alignment pads that one home. */
        spec->size = 1;
        uint32_t declared = declared_record_alignment(module, type);
        if (shape.kind == XR_LIR_TYPE_RECORD && declared) {
            spec->alignment = declared; spec->size = declared;
        }
        return XR_LIR_VALUE_LAYOUT_OK;
    }
    for (uint32_t f = 0; f < count; ++f) {
        XrtValuePlanSpec child;
        if (xrt_value_plan_view(shaped_dependency(set, fields[f].type, fields[f].array_element), &child) != XRT_MANAGED_OK || !child.size)
            return XR_LIR_VALUE_LAYOUT_ARGUMENT;
        if (child.alignment > spec->alignment) spec->alignment = child.alignment;
        if (!align_size(&spec->size, child.alignment) || !extent_add(&spec->size, child.size, 1) ||
            !extent_add(&spec->owned_count, child.owned_count, 1)) return XR_LIR_VALUE_LAYOUT_OVERFLOW;
    }
    if (!align_size(&spec->size, spec->alignment) || !extent_add(scratch_bytes, count, sizeof(XrtValueSlotSpec)) ||
        !extent_add(scratch_bytes, spec->owned_count, sizeof(size_t))) return XR_LIR_VALUE_LAYOUT_OVERFLOW;
    if (shape.kind == XR_LIR_TYPE_RECORD) {
        /* A declared align(N) raises the record above its fields and pads the
         * total size; a promise below the natural alignment cannot be kept. */
        uint32_t declared = declared_record_alignment(module, type);
        if (declared) {
            if (declared < spec->alignment) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
            spec->alignment = declared;
            if (!align_size(&spec->size, declared)) return XR_LIR_VALUE_LAYOUT_OVERFLOW;
        }
    }
    return XR_LIR_VALUE_LAYOUT_OK;
}
static void fill_composite(const XrLirModule *module, const XrLirValueLayouts *set, XrLirType type,
    XrtValuePlanSpec *spec, void *scratch) {
    size_t *owned;
    if (spec->kind == XRT_VALUE_CELL || spec->kind == XRT_VALUE_CLASS) {
        owned = scratch; *owned = 0; spec->owned_offsets = owned;
        return;
    }
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
            parameters[p] = (XrtValueParameterSpec){.type=shaped_dependency(set, parameter.type, parameter.array_element),
                .mode=parameter.mode, .escaping=parameter.escaping};
        }
        return;
    }
    if (spec->kind == XRT_VALUE_SUM) {
        const XrLirRecordField *fields = NULL; uint32_t field_count = 0;
        const XrLirSumVariant *variants = NULL; uint32_t variant_count = 0;
        XR_CHECK(xr_lir_sum_variants(module, type, &fields, &field_count, &variants, &variant_count) == true &&
            field_count == spec->field_count && variant_count == spec->sum.variant_count,
            "sum variant ranges stay borrowed during projection");
        XrtValueSlotSpec *slots = scratch;
        XrtValueSumVariantSpec *sum_variants = (XrtValueSumVariantSpec *)(slots + field_count);
        spec->fields = field_count ? slots : NULL; spec->sum.variants = variant_count ? sum_variants : NULL;
        size_t payload = spec->sum.payload_offset;
        for (uint32_t v = 0; v < variant_count; ++v) {
            sum_variants[v] = (XrtValueSumVariantSpec){variants[v].field_begin, variants[v].field_count};
            size_t end = payload;
            for (uint32_t f = 0; f < variants[v].field_count; ++f) {
                uint32_t at = (uint32_t)(variants[v].field_begin + f);
                const XrLirRecordField *field = &fields[at];
                const XrtValuePlan *plan = dependency(set, field->type);
                XrtValuePlanSpec child;
                XR_CHECK(xrt_value_plan_view(plan, &child) == XRT_MANAGED_OK && child.alignment,
                    "sum field offsets were checked before scratch allocation");
                XR_CHECK(end <= SIZE_MAX - (child.alignment - 1), "sum field alignment overflow was pre-checked");
                size_t offset = (end + child.alignment - 1) & ~(child.alignment - 1);
                slots[at] = (XrtValueSlotSpec){plan, offset, NULL, 0, 0};
                end = offset + child.size;
            }
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
    if (!xrt_value_kind_has_fields(spec->kind)) return;
    const XrLirRecordField *fields = NULL; uint32_t count = 0;
    XR_CHECK(xr_lir_aggregate_fields(module, type, &fields, &count) == XR_LIR_OK, "finished record fields stay borrowed during projection");
    if (!count) return;
    XrtValueSlotSpec *slots = scratch;
    owned = (size_t *)(slots + count);
    spec->fields = slots; spec->owned_offsets = spec->owned_count ? owned : NULL;
    size_t offset = 0, leaves = 0;
    for (uint32_t f = 0; f < count; ++f) {
        const XrtValuePlan *plan = shaped_dependency(set, fields[f].type, fields[f].array_element);
        XrtValuePlanSpec child;
        XR_CHECK(xrt_value_plan_view(plan, &child) == XRT_MANAGED_OK && align_size(&offset, child.alignment),
            "record offsets were checked before scratch allocation");
        bool writable = xr_lir_class_scalar_field_supported(module, fields[f].type) ||
            xr_lir_class_string_field_supported(module, fields[f].type) ||
            (fields[f].type == XR_LIR_ARRAY && xr_lir_class_array_element_supported(fields[f].array_element));
        uint32_t flags = writable && (fields[f].flags & XR_LIR_RECORD_MUTABLE) ? XRT_VALUE_FIELD_MUTABLE : 0;
        slots[f] = (XrtValueSlotSpec){plan, offset, fields[f].name, fields[f].name_length, flags};
        for (size_t i = 0; i < child.owned_count; ++i) owned[leaves++] = offset + child.owned_offsets[i];
        offset += child.size;
    }
    XR_CHECK(leaves == spec->owned_count, "every transitive owner leaf has one physical offset");
}
static XrLirValueLayoutStatus build_row(const XrLirModule *module, XrLirValueLayouts *set, size_t row) {
    if (set->count >= UINT32_MAX) return XR_LIR_VALUE_LAYOUT_OVERFLOW;
    XrtValuePlanSpec spec = {0};
    size_t scratch_bytes = 0, zero = 0;
    uint32_t environment = set->values[row].key.environment_function;
    XrLirValueLayoutStatus status = environment ? environment_spec(module,set,environment,&spec,&scratch_bytes) :
        row < XR_LIR_TYPE_COUNT || row >= XR_LIR_TYPE_COUNT + set->constructed ?
        scalar_spec(set->values[row].key, &spec) : composite_spec(module, set, (XrLirType)row, &spec, &scratch_bytes);
    if (status != XR_LIR_VALUE_LAYOUT_OK) return status;
    if (spec.kind == XRT_VALUE_ARRAY_INLINE) {
        spec.element = dependency(set, set->values[row].key.array_element);
        XR_CHECK(spec.element, "the exact inline Array row plan precedes its Array");
    }
    void *scratch = NULL;
    if (scratch_bytes) {
        status = memory_status(xrt_managed_metadata_allocate(set->billing, scratch_bytes, &scratch));
        if (status != XR_LIR_VALUE_LAYOUT_OK) return status;
    }
    if (environment) fill_environment(module,set,environment,&spec,scratch);
    else if (row >= XR_LIR_TYPE_COUNT && row < XR_LIR_TYPE_COUNT + set->constructed)
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
    size_t array_rows = (size_t)XR_LIR_ARRAY_FAMILY_COUNT + module->inline_array_count;
    if (!extent_add(&rows, array_rows, 1) || !extent_add(&rows,module->function_count,1) ||
        !extent_add(&bytes, rows, sizeof(LayoutRow)) ||
        !extent_add(&bytes, rows, sizeof(LayoutRow *)) ||
        !extent_add(&bytes, module->type_count, sizeof(XrLirOwnershipTypeFact)))
        return XR_LIR_VALUE_LAYOUT_OVERFLOW;
    if (!xrt_billing_owner_retain(billing)) return XR_LIR_VALUE_LAYOUT_REFCOUNT;
    void *memory = NULL;
    XrLirValueLayoutStatus status = memory_status(xrt_managed_metadata_allocate(billing, bytes, &memory));
    if (status != XR_LIR_VALUE_LAYOUT_OK) { xrt_billing_owner_drop(billing); return status; }
    XrLirValueLayouts *set = memory;
    *set = (XrLirValueLayouts){.billing=billing, .bytes=bytes, .rows=rows,
        .constructed=module->type_count,.array_rows=array_rows,.functions=module->function_count};
    set->ordered = (LayoutRow **)(set->values + rows);
    set->types = (XrLirOwnershipTypeFact *)(set->ordered + rows);
    for (size_t i = 0; i < rows; ++i) set->values[i] = (LayoutRow){.key={(XrLirType)i, 0, 0}};
    size_t arrays = XR_LIR_TYPE_COUNT + set->constructed, environments = arrays + array_rows;
    for (size_t i = 0; i < XR_LIR_ARRAY_FAMILY_COUNT; ++i)
        set->values[arrays + i].key = (XrLirValueLayoutKey){XR_LIR_ARRAY, xr_lir_internal_array_element_at((uint32_t)i),0};
    for (uint32_t t = 0; t < module->type_count; ++t) {
        uint32_t ordinal = module->types[t].inline_array_ordinal;
        if (!ordinal) continue;
        XrLirType element = XR_LIR_TYPE_COUNT + t;
        set->values[element].inline_array_ordinal = ordinal;
        set->values[arrays + XR_LIR_ARRAY_FAMILY_COUNT + ordinal - 1].key = (XrLirValueLayoutKey){XR_LIR_ARRAY, element, 0};
    }
    for (uint32_t f = 0; f < module->function_count; ++f)
        set->values[environments + f].key = (XrLirValueLayoutKey){module->functions[f].closure.callable,0,f + 1};
    for (size_t i = 0; i < count && status == XR_LIR_VALUE_LAYOUT_OK; ++i) {
        size_t index = 0;
        status = key_index(set, roots[i], &index);
        if (status == XR_LIR_VALUE_LAYOUT_OK) set->values[index].needed = true;
    }
    if (status == XR_LIR_VALUE_LAYOUT_OK) status = mark_dependencies(module, set);
    // Scalar Array families have no inline dependencies and precede aggregates.
    for (size_t i = arrays; i < arrays + XR_LIR_ARRAY_FAMILY_COUNT && status == XR_LIR_VALUE_LAYOUT_OK; ++i)
        if (set->values[i].needed) status = build_row(module, set, i);
    for (size_t i = 1; i < arrays && status == XR_LIR_VALUE_LAYOUT_OK; ++i) {
        if (set->values[i].needed) status = build_row(module, set, i);
        uint32_t ordinal = set->values[i].inline_array_ordinal;
        if (ordinal && status == XR_LIR_VALUE_LAYOUT_OK) {
            size_t row = arrays + XR_LIR_ARRAY_FAMILY_COUNT + ordinal - 1;
            if (set->values[row].needed) status = build_row(module, set, row);
        }
    }
    for (size_t i = environments; i < rows && status == XR_LIR_VALUE_LAYOUT_OK; ++i)
        if (set->values[i].needed) status = build_row(module,set,i);
    for (size_t i = XR_LIR_TYPE_COUNT; i < arrays && status == XR_LIR_VALUE_LAYOUT_OK; ++i) {
        if (!set->values[i].plan) continue;
        XrtValuePlanSpec spec;
        status = memory_status(xrt_value_plan_view(set->values[i].plan, &spec));
        if (status == XR_LIR_VALUE_LAYOUT_OK && (xrt_value_kind_has_fields(spec.kind) || spec.kind == XRT_VALUE_NULLABLE ||
            spec.kind == XRT_VALUE_CELL || spec.kind == XRT_VALUE_CLASS || spec.kind == XRT_VALUE_SUM)) {
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
