/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_module_layout.inc.c - Independently billed receiving state metadata
 *
 * KEY CONCEPT:
 *   Slots name the same one-based physical plans as C and bytecode. Neither a
 *   compiler arena nor a raw image pointer survives this receiving boundary.
 */
static bool module_layout_storage(XrtModuleLayout *layout, const XrtModuleLayoutSpec *spec, size_t *bytes) {
    uint8_t *base = (uint8_t *)layout;
    void *part = NULL;
    *bytes = sizeof(XrtModuleLayout);
#define MODULE_LAYOUT_PART(field,n,t) do { \
    if (!module_part(base, bytes, (n), sizeof(t), alignof(t), &part)) return false; \
    if (layout) layout->field = part; \
} while (0)
    MODULE_LAYOUT_PART(types, spec->values->type_count, ModuleType);
    MODULE_LAYOUT_PART(slot_counts, spec->module_count, uint32_t);
    MODULE_LAYOUT_PART(view.modules, spec->module_count, XrtModuleSpec);
    MODULE_LAYOUT_PART(view.dependencies, spec->dependency_count, uint32_t);
    MODULE_LAYOUT_PART(view.slots, spec->slot_count, XrtModuleSlotSpec);
#undef MODULE_LAYOUT_PART
    return true;
}
static bool module_layout_shape(const XrtModuleLayoutSpec *spec) {
    if (!spec || !spec->values || !spec->module_count || !spec->modules ||
        (spec->slot_count != 0) != (spec->slots != NULL) ||
        (spec->dependency_count != 0) != (spec->dependencies != NULL) ||
        !spec->payload_alignment || spec->payload_alignment > alignof(XrMaxAlignment) ||
        (spec->payload_alignment & (spec->payload_alignment - 1)) ||
        spec->payload_size % spec->payload_alignment) return false;
    uint32_t first = 0;
    for (uint32_t m = 0; m < spec->module_count; ++m) {
        XrtModuleSpec row = spec->modules[m];
        if (row.first_dependency != first || row.dependency_count > spec->dependency_count - first) return false;
        for (uint32_t d = 0; d < row.dependency_count; ++d) {
            uint32_t target = spec->dependencies[first + d];
            if (target >= spec->module_count || target == m) return false;
        }
        first += row.dependency_count;
    }
    return first == spec->dependency_count;
}
static XrtManagedStatus module_layout_dependencies(XrtModuleLayout *layout) {
    size_t bytes = 0;
    uint32_t count = layout->view.module_count;
    if (!module_extent(&bytes, count, sizeof(uint32_t) * 2 + 1)) return XRT_MANAGED_OVERFLOW;
    void *memory = NULL;
    XrtManagedStatus status = xrt_managed_metadata_allocate(layout->billing, bytes, &memory);
    if (status != XRT_MANAGED_OK) return status;
    memset(memory, 0, bytes);
    uint32_t *next = memory, *stack = next + count;
    uint8_t *color = (uint8_t *)(stack + count);
    for (uint32_t root = 0; root < count && status == XRT_MANAGED_OK; ++root) {
        if (color[root]) continue;
        size_t depth = 1; stack[0] = root; color[root] = 1;
        while (depth) {
            uint32_t current = stack[depth - 1];
            XrtModuleSpec row = layout->view.modules[current];
            if (next[current] == row.dependency_count) { color[current] = 2; --depth; continue; }
            uint32_t child = layout->view.dependencies[row.first_dependency + next[current]++];
            if (color[child] == 1) { status = XRT_MANAGED_ARGUMENT; break; }
            if (color[child] == 2) continue;
            color[child] = 1; stack[depth++] = child;
        }
    }
    xrt_managed_metadata_free(layout->billing, memory, bytes);
    return status;
}
static void module_type_child(ModuleType *type, const ModuleType *child) {
    type->supported &= child->supported;
    if (child->depth >= type->depth) type->depth = child->depth + 1;
}
static XrtManagedStatus module_layout_types(XrtModuleLayout *layout, const XrtValueImage *image) {
    for (uint32_t i = 0; i < image->type_count; ++i) {
        ModuleType *type = &layout->types[i];
        *type = (ModuleType){0};
        XrtManagedStatus status = xrt_value_image_get(layout->plans, i + 1, &type->plan);
        if (status != XRT_MANAGED_OK) return status;
        status = xrt_value_plan_view(type->plan, &type->spec);
        if (status != XRT_MANAGED_OK) return status;
        type->depth = 1;
        type->supported = type->spec.kind <= XRT_VALUE_NULLABLE || type->spec.kind == XRT_VALUE_FUNCTION ||
            type->spec.kind == XRT_VALUE_SET_I64;
        const XrtValueImageType *raw = &image->types[i];
        if (xrt_value_kind_has_fields(type->spec.kind)) {
            for (uint32_t f = 0; f < raw->field_count; ++f)
                module_type_child(type, &layout->types[image->slots[raw->first_field + f].type - 1]);
        } else if (type->spec.kind == XRT_VALUE_NULLABLE)
            module_type_child(type, &layout->types[raw->nullable_element - 1]);
    }
    return XRT_MANAGED_OK;
}
static XrtManagedStatus module_layout_slots(XrtModuleLayout *layout) {
    size_t end = 0;
    for (uint32_t s = 0; s < layout->view.slot_count; ++s) {
        const XrtModuleSlotSpec *slot = &layout->view.slots[s];
        if (!slot->type || slot->type > layout->view.type_count || slot->module >= layout->view.module_count ||
            (slot->flags & ~XRT_MODULE_SLOT_MUTABLE)) return XRT_MANAGED_ARGUMENT;
        const ModuleType *type = &layout->types[slot->type - 1];
        if (!type->supported || slot->size != type->spec.size || slot->alignment != type->spec.alignment ||
            slot->alignment > layout->view.payload_alignment || !module_align(&end, slot->alignment) ||
            slot->offset != end || slot->offset > layout->view.payload_size ||
            slot->size > layout->view.payload_size - slot->offset) return XRT_MANAGED_ARGUMENT;
        end += slot->size;
        ++layout->slot_counts[slot->module];
        if (type->spec.size > layout->scratch_size) layout->scratch_size = type->spec.size;
        if (type->spec.owned_count > layout->max_leaves) layout->max_leaves = type->spec.owned_count;
        if (type->depth > layout->depth) layout->depth = type->depth;
    }
    return module_align(&end, layout->view.payload_alignment) && end == layout->view.payload_size ?
        XRT_MANAGED_OK : XRT_MANAGED_ARGUMENT;
}
XRAY_API bool xrt_module_layout_retain(XrtModuleLayout *layout) {
    return layout && xr_signed_rc_retain(&layout->references);
}
XRAY_API void xrt_module_layout_drop(XrtModuleLayout **owner) {
    if (!owner || !*owner) return;
    XrtModuleLayout *layout = *owner; *owner = NULL;
    XrSignedRcRelease release = xr_signed_rc_release(&layout->references);
    XR_CHECK(release != XR_SIGNED_RC_INVALID, "a module layout has one real reference per owner");
    if (release != XR_SIGNED_RC_LAST) return;
    XrtBillingOwner *billing = layout->billing;
    size_t bytes = layout->bytes;
    xrt_value_image_drop(&layout->plans);
    xrt_managed_metadata_free(billing, layout, bytes);
    xrt_billing_owner_drop(billing);
}
XRAY_API XrtModuleStatus xrt_module_layout_new(XrtBillingOwner *billing, const XrtModuleLayoutSpec *spec,
    XrtManagedLease code, XrtModuleLayout **out) {
    if (!billing || !out || *out || !((code.retain && code.drop) ||
        (!code.context && !code.retain && !code.drop))) return XRT_MANAGED_ARGUMENT;
    if (code.retain && !code.retain(code.context)) return XRT_MANAGED_REFCOUNT;
    XrtManagedStatus status = module_layout_shape(spec) ? XRT_MANAGED_OK : XRT_MANAGED_ARGUMENT;
    XrtModuleLayout *layout = NULL;
    size_t bytes = 0;
    if (status == XRT_MANAGED_OK && !module_layout_storage(NULL, spec, &bytes)) status = XRT_MANAGED_OVERFLOW;
    if (status == XRT_MANAGED_OK) {
        if (!xrt_billing_owner_retain(billing)) status = XRT_MANAGED_REFCOUNT;
        else {
            void *memory = NULL;
            status = xrt_managed_metadata_allocate(billing, bytes, &memory);
            if (status != XRT_MANAGED_OK) xrt_billing_owner_drop(billing);
            else {
                memset(memory, 0, bytes);
                layout = memory;
                *layout = (XrtModuleLayout){.billing=billing, .bytes=bytes, .depth=1};
                atomic_init(&layout->references, -1);
                layout->view = (XrtModuleLayoutView){.type_count=spec->values->type_count,
                    .module_count=spec->module_count, .dependency_count=spec->dependency_count,
                    .slot_count=spec->slot_count, .payload_size=spec->payload_size,
                    .payload_alignment=spec->payload_alignment};
                bool valid = module_layout_storage(layout, spec, &bytes);
                XR_CHECK(valid && bytes == layout->bytes, "layout vectors match their reserved extent");
                memcpy((void *)layout->view.modules, spec->modules, (size_t)spec->module_count * sizeof(*spec->modules));
                if (spec->dependency_count) memcpy((void *)layout->view.dependencies, spec->dependencies,
                    (size_t)spec->dependency_count * sizeof(*spec->dependencies));
                if (spec->slot_count) memcpy((void *)layout->view.slots, spec->slots,
                    (size_t)spec->slot_count * sizeof(*spec->slots));
                status = xrt_value_image_bind(billing, spec->values, code, &layout->plans);
                if (status == XRT_MANAGED_OK) status = module_layout_types(layout, spec->values);
                if (status == XRT_MANAGED_OK) status = module_layout_slots(layout);
                if (status == XRT_MANAGED_OK) status = module_layout_dependencies(layout);
            }
        }
    }
    if (code.drop) code.drop(code.context);
    if (status == XRT_MANAGED_OK) { *out = layout; layout = NULL; }
    xrt_module_layout_drop(&layout);
    return status;
}
XRAY_API XrtModuleStatus xrt_module_layout_view(const XrtModuleLayout *layout, XrtModuleLayoutView *out) {
    if (!layout || !out) return XRT_MANAGED_ARGUMENT;
    *out = layout->view;
    return XRT_MANAGED_OK;
}
XR_FUNCDEF XrtManagedStatus xrt_module_layout_plan(const XrtModuleLayout *layout, uint32_t type,
    const XrtValuePlan **empty) {
    if (!layout || !empty || *empty || !type || type > layout->view.type_count) return XRT_MANAGED_ARGUMENT;
    *empty = layout->types[type - 1].plan;
    return XRT_MANAGED_OK;
}
