/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_module_layout.inc.c - Retained immutable type and slot catalogs
 *
 * KEY CONCEPT:
 *   Validated numeric rows select fixed runtime operations. Every type context
 *   and dependency row remains alive through the retained layout owner.
 */
static XrXirValueStatus module_layout_shape(const XrtModuleLayoutSpec *spec, uint64_t *bytes) {
    if (!spec || !spec->module_count || !spec->modules ||
        (spec->type_count != 0) != (spec->types != NULL) ||
        (spec->slot_count != 0) != (spec->slots != NULL) ||
        (spec->dependency_count != 0) != (spec->dependencies != NULL) ||
        (spec->callable_type_count != 0) != (spec->callable_types != NULL) ||
        !spec->payload_alignment || (spec->payload_alignment & (spec->payload_alignment - 1)) ||
        spec->payload_size % spec->payload_alignment || spec->payload_size > SIZE_MAX)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    *bytes = sizeof(XrtModuleLayout);
    if (!module_charge(bytes, spec->type_count, sizeof(XrtModuleTypeSpec) + sizeof(XrtModuleType)) ||
        !module_charge(bytes, spec->module_count, sizeof(XrtModuleSpec) + sizeof(uint32_t)) ||
        !module_charge(bytes, spec->dependency_count, sizeof(uint32_t)) ||
        !module_charge(bytes, spec->slot_count, sizeof(XrtModuleSlotSpec))) return XR_XIR_VALUE_OVERFLOW;
    uint64_t peak = *bytes;
    if (!module_charge(&peak, spec->module_count, 1 + 2 * sizeof(uint32_t))) return XR_XIR_VALUE_OVERFLOW;
    if (peak > spec->byte_limit) return XR_XIR_VALUE_MEMORY_LIMIT;
    uint32_t dependency = 0;
    for (uint32_t m = 0; m < spec->module_count; ++m) {
        XrtModuleSpec module = spec->modules[m];
        if (module.first_dependency != dependency ||
            module.dependency_count > spec->dependency_count - dependency) return XR_XIR_VALUE_BAD_ARGUMENT;
        for (uint32_t d = 0; d < module.dependency_count; ++d) {
            uint32_t target = spec->dependencies[dependency + d];
            if (target >= spec->module_count || target == m) return XR_XIR_VALUE_BAD_ARGUMENT;
        }
        dependency += module.dependency_count;
    }
    if (dependency != spec->dependency_count) return XR_XIR_VALUE_BAD_ARGUMENT;
    return XR_XIR_VALUE_OK;
}

static XrXirValueStatus module_layout_slots(const XrtModuleLayout *layout) {
    const XrtModuleLayoutSpec *spec = &layout->view;
    uint64_t end = 0;
    for (uint32_t s = 0; s < spec->slot_count; ++s) {
        const XrtModuleSlotSpec *slot = &spec->slots[s];
        if (slot->module >= spec->module_count || slot->type >= spec->type_count ||
            (slot->flags & ~XRT_MODULE_SLOT_MUTABLE)) return XR_XIR_VALUE_BAD_ARGUMENT;
        XrtModuleType type = layout->types[slot->type];
        if (type.kind == XRT_MODULE_CELL_ARRAY_I64 && !(slot->flags & XRT_MODULE_SLOT_MUTABLE))
            return XR_XIR_VALUE_BAD_ARGUMENT;
        if (slot->size != type.size || slot->alignment != type.alignment ||
            slot->alignment > spec->payload_alignment || slot->offset % type.alignment ||
            slot->offset < end || slot->offset > spec->payload_size ||
            slot->size > spec->payload_size - slot->offset) return XR_XIR_VALUE_BAD_ARGUMENT;
        end = slot->offset + slot->size;
    }
    return XR_XIR_VALUE_OK;
}

static XrXirValueStatus module_layout_dependencies(const XrtModuleLayout *layout) {
    uint32_t count = layout->view.module_count;
    uint8_t *color = xr_calloc(count, 1);
    uint32_t *next = xr_calloc(count, sizeof(*next));
    uint32_t *stack = xr_malloc((size_t)count * sizeof(*stack));
    if (!color || !next || !stack) {
        xr_free(color); xr_free(next); xr_free(stack);
        return XR_XIR_VALUE_OOM;
    }
    bool acyclic = true;
    for (uint32_t root = 0; root < count && acyclic; ++root) {
        if (color[root]) continue;
        uint32_t depth = 1;
        stack[0] = root; color[root] = 1;
        while (depth && acyclic) {
            uint32_t current = stack[depth - 1];
            XrtModuleSpec module = layout->view.modules[current];
            if (next[current] == module.dependency_count) { color[current] = 2; --depth; continue; }
            uint32_t target = layout->view.dependencies[module.first_dependency + next[current]++];
            if (color[target] == 1) { acyclic = false; break; }
            if (color[target] == 2) continue;
            color[target] = 1; stack[depth++] = target;
        }
    }
    xr_free(color); xr_free(next); xr_free(stack);
    return acyclic ? XR_XIR_VALUE_OK : XR_XIR_VALUE_BAD_ARGUMENT;
}

XRAY_API bool xrt_module_layout_retain(XrtModuleLayout *layout) {
    if (!layout) return false;
    size_t references = atomic_load_explicit(&layout->references, memory_order_relaxed);
    while (references && references != SIZE_MAX)
        if (atomic_compare_exchange_weak_explicit(&layout->references, &references,
            references + 1, memory_order_relaxed, memory_order_relaxed)) return true;
    return false;
}

XRAY_API void xrt_module_layout_drop(XrtModuleLayout **owner) {
    if (!owner || !*owner) return;
    XrtModuleLayout *layout = *owner;
    *owner = NULL;
    size_t previous = atomic_fetch_sub_explicit(&layout->references, 1, memory_order_acq_rel);
    XR_CHECK(previous, "A module layout remains live until its last owner retires");
    if (previous != 1) return;
    xr_free((void *)layout->view.types); xr_free((void *)layout->view.modules);
    xr_free((void *)layout->view.dependencies); xr_free((void *)layout->view.slots);
    xr_xir_compile_type_arena_drop(layout->array_types);
    xr_xir_compile_type_arena_drop(layout->callable_types);
    xr_xir_compile_type_arena_drop(layout->map_types);
    if (layout->view.callable_types) {
        for (uint32_t t = 0; t < layout->view.callable_type_count; ++t)
            xr_free((void *)layout->view.callable_types[t].parameters);
        xr_free((void *)layout->view.callable_types);
    }
    xr_free(layout->types); xr_free(layout->slot_counts); xr_free(layout);
}

XRAY_API XrtModuleStatus xrt_module_layout_new(const XrtModuleLayoutSpec *spec, XrtModuleLayout **out) {
    if (!out || *out) return XR_XIR_VALUE_BAD_ARGUMENT;
    uint64_t bytes;
    XrXirValueStatus status = module_layout_shape(spec, &bytes);
    if (status != XR_XIR_VALUE_OK) return status;
    XrtModuleLayout *layout = xr_calloc(1, sizeof(*layout));
    if (!layout) return XR_XIR_VALUE_OOM;
    atomic_init(&layout->references, 1);
    layout->bytes = bytes;
    layout->view = *spec;
    layout->view.callable_types = NULL;
    layout->view.types = module_copy_array(spec->types, spec->type_count, sizeof(*spec->types));
    layout->view.modules = module_copy_array(spec->modules, spec->module_count, sizeof(*spec->modules));
    layout->view.dependencies = module_copy_array(spec->dependencies, spec->dependency_count, sizeof(*spec->dependencies));
    layout->view.slots = module_copy_array(spec->slots, spec->slot_count, sizeof(*spec->slots));
    layout->types = spec->type_count ? xr_calloc(spec->type_count, sizeof(*layout->types)) : NULL;
    layout->slot_counts = xr_calloc(spec->module_count, sizeof(*layout->slot_counts));
    if ((spec->type_count && (!layout->view.types || !layout->types)) || !layout->view.modules ||
        (spec->dependency_count && !layout->view.dependencies) || (spec->slot_count && !layout->view.slots) ||
        !layout->slot_counts) { xrt_module_layout_drop(&layout); return XR_XIR_VALUE_OOM; }
    if (spec->callable_type_count) {
        uint64_t callable_bytes = layout->bytes;
        if (!module_charge(&callable_bytes, spec->callable_type_count, sizeof(XrtCallableTypeSpec))) {
            xrt_module_layout_drop(&layout); return XR_XIR_VALUE_OVERFLOW;
        }
        XrtCallableTypeSpec *types = xr_calloc(spec->callable_type_count, sizeof(*types));
        if (!types) { xrt_module_layout_drop(&layout); return XR_XIR_VALUE_OOM; }
        layout->view.callable_types = types;
        for (uint32_t t = 0; t < spec->callable_type_count; ++t) {
            XrtCallableTypeSpec from = spec->callable_types[t];
            if ((from.parameter_count != 0) != (from.parameters != NULL) ||
                !module_charge(&callable_bytes, from.parameter_count, sizeof(*from.parameters))) {
                xrt_module_layout_drop(&layout); return XR_XIR_VALUE_BAD_ARGUMENT;
            }
            types[t] = from;
            types[t].parameters = module_copy_array(from.parameters, from.parameter_count, sizeof(*from.parameters));
            if (from.parameter_count && !types[t].parameters) {
                xrt_module_layout_drop(&layout); return XR_XIR_VALUE_OOM;
            }
        }
        if (callable_bytes > spec->byte_limit) { xrt_module_layout_drop(&layout); return XR_XIR_VALUE_MEMORY_LIMIT; }
        layout->bytes = callable_bytes;
        uint64_t arena_bytes = 0;
        status = xrt_callable_types_new_accounted(types, spec->callable_type_count,
            spec->byte_limit - callable_bytes, &layout->callable_types, &arena_bytes);
        if (status == XR_XIR_VALUE_OK && !module_charge(&layout->bytes, arena_bytes, 1))
            status = XR_XIR_VALUE_OVERFLOW;
        if (status != XR_XIR_VALUE_OK) { xrt_module_layout_drop(&layout); return status; }
    }
    for (uint32_t t = 0; t < spec->type_count; ++t) {
        if (!module_type(&layout->view, t, layout->types)) {
            xrt_module_layout_drop(&layout); return XR_XIR_VALUE_BAD_ARGUMENT;
        }
    }
    status = module_layout_slots(layout);
    if (status != XR_XIR_VALUE_OK) { xrt_module_layout_drop(&layout); return status; }
    for (uint32_t s = 0; s < spec->slot_count; ++s) {
        const XrtModuleSlotSpec *slot = &layout->view.slots[s];
        if (layout->types[slot->type].copy == module_array_copy && !layout->array_types) {
            uint64_t arena_bytes = 0;
            status = xrt_array_types_new(spec->byte_limit - layout->bytes, &layout->array_types, &arena_bytes);
            if (status == XR_XIR_VALUE_OK && !module_charge(&layout->bytes, arena_bytes, 1))
                status = XR_XIR_VALUE_OVERFLOW;
            if (status != XR_XIR_VALUE_OK) { xrt_module_layout_drop(&layout); return status; }
        }
        if (layout->types[slot->type].copy == module_map_copy && !layout->map_types) {
            uint64_t arena_bytes = 0;
            status = xrt_map_types_new(spec->byte_limit - layout->bytes, &layout->map_types, &arena_bytes);
            if (status == XR_XIR_VALUE_OK && !module_charge(&layout->bytes, arena_bytes, 1))
                status = XR_XIR_VALUE_OVERFLOW;
            if (status != XR_XIR_VALUE_OK) { xrt_module_layout_drop(&layout); return status; }
        }
        ++layout->slot_counts[slot->module];
        if (slot->size > layout->scratch_size) layout->scratch_size = slot->size;
    }
    for (uint32_t t = 0; t < spec->type_count; ++t)
        layout->types[t].arena = layout->types[t].kind == XRT_MODULE_CALLABLE ? layout->callable_types :
            layout->types[t].kind == XRT_MODULE_MAP_STRING_I64 ? layout->map_types : layout->array_types;
    uint64_t peak = layout->bytes;
    if (!module_charge(&peak, spec->module_count, 1 + 2 * sizeof(uint32_t)) || peak > spec->byte_limit) {
        xrt_module_layout_drop(&layout); return XR_XIR_VALUE_MEMORY_LIMIT;
    }
    status = module_layout_dependencies(layout);
    if (status != XR_XIR_VALUE_OK) { xrt_module_layout_drop(&layout); return status; }
    *out = layout;
    return XR_XIR_VALUE_OK;
}

XRAY_API XrtModuleStatus xrt_module_layout_view(const XrtModuleLayout *layout, XrtModuleLayoutSpec *out) {
    if (!layout || !out) return XR_XIR_VALUE_BAD_ARGUMENT;
    *out = layout->view;
    return XR_XIR_VALUE_OK;
}
