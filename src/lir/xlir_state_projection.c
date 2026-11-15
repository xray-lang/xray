/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_state_projection.c - Metered raw State placement from common value plans
 *
 * KEY CONCEPT:
 *   Only complete binding placement is computed here. Records, Nullable and
 *   flattened owner fields come exclusively from the retained physical set.
 */
#include "xlir_state_projection.h"
#include "xlir_types.h"
#include "xlir_internal.h"
#include "base/xchecks.h"
#include <stdalign.h>
#include <string.h>

struct XrLirStateProjection {
    XrtBillingOwner *billing;
    size_t bytes;
    XrtModuleLayoutSpec spec;
};
static bool state_extent(size_t *bytes, size_t count, size_t stride) {
    if (stride && count > (SIZE_MAX - *bytes) / stride) return false;
    *bytes += count * stride; return true;
}
static bool state_align(size_t *bytes, size_t alignment) {
    if (!alignment || (alignment & (alignment - 1)) || *bytes > SIZE_MAX - (alignment - 1)) return false;
    *bytes = (*bytes + alignment - 1) & ~(alignment - 1); return true;
}
static XrLirValueLayoutStatus state_memory(XrtManagedStatus status) {
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
static XrLirValueLayoutStatus state_key(const XrLirModule *module, const XrLirStateSlot *slot,
    XrLirValueLayoutKey *key) {
    XrLirType type = slot->payload;
    bool supported = type >= XR_LIR_UNIT && type <= XR_LIR_F64;
    if (type >= XR_LIR_TYPE_COUNT) {
        XrLirTypeView shape;
        if (xr_lir_type_view(module,type,&shape) != XR_LIR_OK) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
        supported = shape.kind == XR_LIR_TYPE_RECORD || shape.kind == XR_LIR_TYPE_NULLABLE ||
            shape.kind == XR_LIR_TYPE_FIXED_ARRAY || shape.kind == XR_LIR_TYPE_TUPLE || shape.kind == XR_LIR_TYPE_CALLABLE;
    }
    if (!supported && type != XR_LIR_STRING && type != XR_LIR_ARRAY && type != XR_LIR_MAP_STRING_I64)
        return XR_LIR_VALUE_LAYOUT_UNSUPPORTED;
    *key = (XrLirValueLayoutKey){type,type == XR_LIR_ARRAY ? slot->array_element : 0,0};
    return XR_LIR_VALUE_LAYOUT_OK;
}
XR_FUNCDEF void xr_lir_state_projection_destroy(XrLirStateProjection *projection) {
    if (!projection) return;
    XrtBillingOwner *billing = projection->billing;
    size_t bytes = projection->bytes;
    xrt_managed_metadata_free(billing,projection,bytes);
    xrt_billing_owner_drop(billing);
}
XR_FUNCDEF const XrtModuleLayoutSpec *xr_lir_state_projection_view(const XrLirStateProjection *projection) {
    return projection ? &projection->spec : NULL;
}
XR_FUNCDEF XrLirValueLayoutStatus xr_lir_state_projection_create(const XrLirModule *module,
    const XrLirValueLayouts *layouts, const XrtValueImage *image, XrtBillingOwner *billing,
    XrLirStateProjection **empty) {
    if (!module || !module->finished || module->failure != XR_LIR_OK || !billing || !empty || *empty)
        return XR_LIR_VALUE_LAYOUT_ARGUMENT;
    XrLirStateLayout source = {0};
    if (xr_lir_state_layout_view(module,&source) != XR_LIR_OK) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
    if (!source.module_count) return XR_LIR_VALUE_LAYOUT_OK;
    if (!layouts || !image || xr_lir_value_layouts_count(layouts) != image->type_count)
        return XR_LIR_VALUE_LAYOUT_ARGUMENT;
    size_t modules = sizeof(XrLirStateProjection), dependencies, slots, bytes;
    if (!state_align(&modules,alignof(XrtModuleSpec))) return XR_LIR_VALUE_LAYOUT_OVERFLOW;
    dependencies = modules;
    if (!state_extent(&dependencies,source.module_count,sizeof(XrtModuleSpec)) ||
        !state_align(&dependencies,alignof(uint32_t))) return XR_LIR_VALUE_LAYOUT_OVERFLOW;
    slots = dependencies;
    if (!state_extent(&slots,source.dependency_count,sizeof(uint32_t)) ||
        !state_align(&slots,alignof(XrtModuleSlotSpec))) return XR_LIR_VALUE_LAYOUT_OVERFLOW;
    bytes = slots;
    if (!state_extent(&bytes,source.slot_count,sizeof(XrtModuleSlotSpec))) return XR_LIR_VALUE_LAYOUT_OVERFLOW;
    if (!xrt_billing_owner_retain(billing)) return XR_LIR_VALUE_LAYOUT_REFCOUNT;
    void *memory = NULL;
    XrLirValueLayoutStatus status = state_memory(xrt_managed_metadata_allocate(billing,bytes,&memory));
    if (status != XR_LIR_VALUE_LAYOUT_OK) { xrt_billing_owner_drop(billing); return status; }
    XrLirStateProjection *projection = memory;
    *projection = (XrLirStateProjection){.billing=billing,.bytes=bytes};
    XrtModuleSpec *module_rows = (XrtModuleSpec *)((uint8_t *)memory + modules);
    uint32_t *dependency_rows = (uint32_t *)((uint8_t *)memory + dependencies);
    XrtModuleSlotSpec *slot_rows = (XrtModuleSlotSpec *)((uint8_t *)memory + slots);
    projection->spec = (XrtModuleLayoutSpec){image,module_rows,source.module_count,
        source.dependency_count ? dependency_rows : NULL,source.dependency_count,
        source.slot_count ? slot_rows : NULL,source.slot_count,0,1};
    for (uint32_t i = 0; i < source.module_count; ++i)
        module_rows[i] = (XrtModuleSpec){source.modules[i].first_dependency,source.modules[i].dependency_count};
    if (source.dependency_count) memcpy(dependency_rows,source.dependencies,
        (size_t)source.dependency_count * sizeof(*dependency_rows));
    size_t size = 0, alignment = 1;
    for (uint32_t i = 0; i < source.slot_count && status == XR_LIR_VALUE_LAYOUT_OK; ++i) {
        const XrLirStateSlot *slot = &source.slots[i];
        XrLirValueLayoutKey key; const XrtValuePlan *plan = NULL; uint32_t id = 0;
        status = state_key(module,slot,&key);
        if (status != XR_LIR_VALUE_LAYOUT_OK) break;
        status = xr_lir_value_layouts_lookup(layouts,key,&plan,&id);
        if (status != XR_LIR_VALUE_LAYOUT_OK) break;
        XrtValuePlanSpec spec;
        if (xrt_value_plan_view(plan,&spec) != XRT_MANAGED_OK || !id) { status = XR_LIR_VALUE_LAYOUT_ARGUMENT; break; }
        if (!state_align(&size,spec.alignment)) { status = XR_LIR_VALUE_LAYOUT_OVERFLOW; break; }
        slot_rows[i] = (XrtModuleSlotSpec){slot->module,id,slot->mutable ? XRT_MODULE_SLOT_MUTABLE : 0,
            size,spec.size,spec.alignment};
        if (!state_extent(&size,spec.size,1)) { status = XR_LIR_VALUE_LAYOUT_OVERFLOW; break; }
        if (spec.alignment > alignment) alignment = spec.alignment;
    }
    if (status == XR_LIR_VALUE_LAYOUT_OK && !state_align(&size,alignment)) status = XR_LIR_VALUE_LAYOUT_OVERFLOW;
    if (status == XR_LIR_VALUE_LAYOUT_OK) {
        projection->spec.payload_size = size; projection->spec.payload_alignment = alignment;
        *empty = projection;
    } else xr_lir_state_projection_destroy(projection);
    return status;
}
