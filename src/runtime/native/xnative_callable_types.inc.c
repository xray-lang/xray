/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_callable_types.inc.c - One owned projection of precise physical types
 */
XR_FUNC bool xrt_callable_type_id(XrtCallableValueSpec value, uint32_t count, XrXirType *output) {
    if (!output) return false;
    static const XrXirType scalars[] = {XR_XIR_UNIT, XR_XIR_BOOL,
        XR_XIR_I8, XR_XIR_I16, XR_XIR_I32, XR_XIR_I64, XR_XIR_U8, XR_XIR_U16,
        XR_XIR_U32, XR_XIR_U64, XR_XIR_F32, XR_XIR_F64};
    XrXirType type;
    switch (value.kind) {
    case XRT_CALLABLE_SCALAR:
        if (value.type >= XR_POD_SCALAR_COUNT) return false;
        type = scalars[value.type];
        break;
    case XRT_CALLABLE_STRING:
        if (value.type) return false;
        type = XR_XIR_STRING;
        break;
    case XRT_CALLABLE_ARRAY:
        if (value.type >= 4) return false;
        type = (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE + value.type);
        break;
    case XRT_CALLABLE_MAP_STRING_I64:
        if (value.type) return false;
        type = (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE + 4u);
        break;
    case XRT_CALLABLE_CONSTRUCTED:
        if (value.type >= count || value.type >= XR_XIR_CONSTRUCTED_TYPE_LIMIT - XR_XIR_CONSTRUCTED_TYPE_BASE - XRT_CALLABLE_PREFIX_COUNT)
            return false;
        type = (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE + XRT_CALLABLE_PREFIX_COUNT + value.type);
        break;
    default: return false;
    }
    *output = type;
    return true;
}
XR_FUNC bool xrt_callable_specs_equal(const XrtCallableTypeSpec *left, const XrtCallableTypeSpec *right, uint32_t count) {
    if (count && (!left || !right)) return false;
    for (uint32_t t = 0; t < count; ++t) {
        XrtCallableTypeSpec a = left[t], b = right[t];
        if (a.kind != b.kind || a.element.kind != b.element.kind || a.element.type != b.element.type ||
            a.count != b.count || a.result.kind != b.result.kind || a.result.type != b.result.type ||
            a.flags != b.flags || a.throws != b.throws || a.parameter_count != b.parameter_count ||
            (a.parameter_count && (!a.parameters || !b.parameters))) return false;
        for (uint32_t p = 0; p < a.parameter_count; ++p) {
            XrtCallableParameterSpec x = a.parameters[p], y = b.parameters[p];
            if (x.value.kind != y.value.kind || x.value.type != y.value.type || x.mode != y.mode || x.escaping != y.escaping)
                return false;
        }
    }
    return true;
}

static XrXirValueStatus callable_resource_status(XrCompileResourceStatus status) {
    return status == XR_COMPILE_RESOURCE_MEMORY_LIMIT ? XR_XIR_VALUE_MEMORY_LIMIT :
        status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_VALUE_OOM :
        status == XR_COMPILE_RESOURCE_OVERFLOW ? XR_XIR_VALUE_OVERFLOW : XR_XIR_VALUE_BAD_ARGUMENT;
}

XR_FUNC XrXirValueStatus xrt_callable_types_new_accounted(const XrtCallableTypeSpec *specs, uint32_t count,
    uint64_t limit, XrXirTypeArena **empty, uint64_t *bytes) {
    if (!empty || *empty || !bytes || (count && !specs)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (count > XR_XIR_CONSTRUCTED_TYPE_LIMIT - XR_XIR_CONSTRUCTED_TYPE_BASE - XRT_CALLABLE_PREFIX_COUNT)
        return XR_XIR_VALUE_LIMIT;
    size_t parameter_count = 0;
    for (uint32_t t = 0; t < count; ++t) {
        XrtCallableTypeSpec spec = specs[t];
        if ((uint32_t)spec.kind > XRT_CALLABLE_TUPLE_I64 || spec.parameter_count > 65536 ||
            (spec.parameter_count && !spec.parameters)) return XR_XIR_VALUE_BAD_ARGUMENT;
        if (spec.throws) return XR_XIR_VALUE_UNSUPPORTED;
        if (spec.parameter_count > SIZE_MAX - parameter_count) return XR_XIR_VALUE_OVERFLOW;
        parameter_count += spec.parameter_count;
        if (spec.kind == XRT_CALLABLE_TUPLE_I64) {
            if (!spec.count || spec.count > 65536 || spec.count > SIZE_MAX - parameter_count)
                return XR_XIR_VALUE_BAD_ARGUMENT;
            parameter_count += spec.count;
        }
    }
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_FINITE, limit, true};
    if (limit == UINT64_MAX) limits = (XrCompileResourceLimits){XR_COMPILE_MEMORY_ORDINARY, 0, true};
    XrXirCompileContext context = {.limits=xr_xir_compile_default_limits()};
    XrCompileResourceStatus resource = xr_compile_resources_new(&limits, &context.resources);
    if (resource != XR_COMPILE_RESOURCE_OK) return callable_resource_status(resource);
    XrXirTypeNode *nodes = NULL;
    XrXirCallableParameter *parameters = NULL;
    void *storage = NULL;
    resource = xr_compile_resources_calloc(context.resources, (size_t)count + XRT_CALLABLE_PREFIX_COUNT, sizeof(*nodes), &storage);
    nodes = storage;
    if (resource == XR_COMPILE_RESOURCE_OK && parameter_count) {
        storage = NULL;
        resource = xr_compile_resources_calloc(context.resources, parameter_count, sizeof(*parameters), &storage);
        parameters = storage;
    }
    XrXirValueStatus status = resource == XR_COMPILE_RESOURCE_OK ? XR_XIR_VALUE_OK : callable_resource_status(resource);
    static const XrXirType elements[] = {XR_XIR_I64, XR_XIR_STRING, XR_XIR_U8, XR_XIR_U64};
    if (status == XR_XIR_VALUE_OK)
        for (uint32_t a = 0; a < 4; ++a) nodes[a] = (XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY, .element=elements[a]};
    if (status == XR_XIR_VALUE_OK)
        nodes[4] = (XrXirTypeNode){.kind=XR_XIR_TYPE_MAP, .element=XR_XIR_STRING, .result=XR_XIR_I64};
    size_t next = 0;
    for (uint32_t t = 0; t < count && status == XR_XIR_VALUE_OK; ++t) {
        XrtCallableTypeSpec spec = specs[t];
        XrXirTypeNode *node = &nodes[t + XRT_CALLABLE_PREFIX_COUNT];
        if (spec.kind == XRT_CALLABLE_SIGNATURE) {
            if (spec.count || spec.element.kind != XRT_CALLABLE_SCALAR || spec.element.type ||
                (spec.flags & ~3u) || ((spec.flags & 2u) && !(spec.flags & 1u)) ||
                !xrt_callable_type_id(spec.result, t, &node->result)) {
                status = XR_XIR_VALUE_BAD_ARGUMENT;
                break;
            }
            node->kind = XR_XIR_TYPE_CALLABLE;
            node->element = XR_XIR_NEVER;
            node->flags = XR_XIR_CALLABLE_ROOT_UNRESOLVED |
                (spec.flags & 1u ? XR_XIR_CALLABLE_NO_SUSPEND : 0u) |
                (spec.flags & 2u ? XR_XIR_CALLABLE_C_ABI : 0u);
            node->parameters = spec.parameter_count ? parameters + next : NULL;
            node->parameter_count = spec.parameter_count;
            for (uint32_t p = 0; p < spec.parameter_count; ++p) {
                XrtCallableParameterSpec parameter = spec.parameters[p];
                XrXirCallableParameter *received = &parameters[next++];
                if (parameter.mode > (uint32_t)XR_PARAM_MOVE ||
                    !xrt_callable_type_id(parameter.value, t, &received->type)) {
                    status = XR_XIR_VALUE_BAD_ARGUMENT;
                    break;
                }
                received->mode = parameter.mode;
                received->escaping = parameter.escaping;
            }
        } else {
            if (spec.parameters || spec.parameter_count || spec.flags || spec.result.kind != XRT_CALLABLE_SCALAR ||
                spec.result.type || !xrt_callable_type_id(spec.element, t, &node->element)) {
                status = XR_XIR_VALUE_BAD_ARGUMENT;
                break;
            }
            if (spec.kind == XRT_CALLABLE_FIXED_BYTES) {
                if (!spec.count || spec.count > 65535 || node->element != XR_XIR_U8) {
                    status = XR_XIR_VALUE_BAD_ARGUMENT;
                    break;
                }
                node->kind = XR_XIR_TYPE_FIXED_ARRAY;
                node->parameter_count = spec.count;
            } else if (spec.kind == XRT_CALLABLE_TUPLE_I64) {
                if (node->element != XR_XIR_I64) { status = XR_XIR_VALUE_BAD_ARGUMENT; break; }
                node->kind = XR_XIR_TYPE_TUPLE;
                node->element = XR_XIR_UNIT;
                node->parameters = parameters + next;
                node->parameter_count = spec.count;
                for (uint32_t p = 0; p < spec.count; ++p)
                    parameters[next++].type = XR_XIR_I64;
            } else if (spec.count) status = XR_XIR_VALUE_BAD_ARGUMENT;
            else node->kind = spec.kind == XRT_CALLABLE_NULLABLE ? XR_XIR_TYPE_NULLABLE : XR_XIR_TYPE_CELL;
        }
    }
    if (status == XR_XIR_VALUE_OK) {
        XrXirTypes types = {.nodes=nodes, .count=count + XRT_CALLABLE_PREFIX_COUNT};
        status = xr_xir_compile_type_arena_new(&context, &types, empty);
    }
    XrXirTypeArena *owned_map = NULL;
    const XrXirTypeArena *map_types = NULL;
    uint64_t map_bytes = 0;
    if (status == XR_XIR_VALUE_OK) {
        if (xrt_value_root_active()) status = xrt_map_root_types_ensure(&map_types);
        else {
            status = xrt_map_types_new(limit, &owned_map, &map_bytes);
            map_types = owned_map;
        }
        if (status == XR_XIR_VALUE_OK)
            status = xir_native_type_arena_string_i64_map_bytes(map_types, &map_bytes);
        if (status == XR_XIR_VALUE_OK)
            status = xir_native_type_arena_receive_string_i64_map(*empty,
                XR_XIR_CONSTRUCTED_TYPE_BASE + 4u, map_types);
    }
    xr_xir_compile_type_arena_drop(owned_map);
    xr_compile_resources_free(parameters);
    xr_compile_resources_free(nodes);
    if (status == XR_XIR_VALUE_OK) {
        XrCompileResourceStats stats = {0};
        if (xr_compile_resources_stats(context.resources, &stats) != XR_COMPILE_RESOURCE_OK) {
            xr_xir_compile_type_arena_drop(*empty); *empty = NULL;
            status = XR_XIR_VALUE_BAD_ARGUMENT;
        } else if (map_bytes > limit || stats.held_bytes > limit - map_bytes) status = XR_XIR_VALUE_MEMORY_LIMIT;
        else *bytes = stats.held_bytes + map_bytes;
    }
    if (status != XR_XIR_VALUE_OK) { xr_xir_compile_type_arena_drop(*empty); *empty = NULL; }
    xr_compile_resources_release(context.resources);
    return status;
}
XR_FUNC const XrXirTypeArena *xrt_callable_map_catalog(const XrXirTypeArena *types) {
    return xir_native_type_arena_string_i64_map_source(types, XR_XIR_CONSTRUCTED_TYPE_BASE + 4u);
}
XR_FUNC XrXirValueStatus xrt_callable_types_new(const XrtCallableTypeSpec *specs, uint32_t count,
    uint64_t limit, XrXirTypeArena **empty) {
    uint64_t bytes = 0;
    return xrt_callable_types_new_accounted(specs, count, limit, empty, &bytes);
}
