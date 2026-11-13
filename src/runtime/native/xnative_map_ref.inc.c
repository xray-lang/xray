/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_map_ref.inc.c - Synchronous access to current Map bindings
 *
 * KEY CONCEPT:
 *   Every operation resolves the actual local or module payload. State
 *   readiness and mutability are checked at the operation boundary.
 */
XRAY_API XrtMapRef xrt_map_ref_local(XrtMap **cell, uint32_t source) {
    if (!cell) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    return (XrtMapRef){.cell=cell, .state={.kind=XRT_MODULE_MAP_STRING_I64}};
}
XRAY_API XrtMapRef xrt_map_ref_state(XrtModuleRef *state, uint32_t source) {
    XrtMapRef reference = {0};
    xrt_module_require(xrt_module_reference_cache(state, &reference.state), source);
    if (reference.state.kind != XRT_MODULE_MAP_STRING_I64 || !reference.state.is_mutable)
        xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    return reference;
}
static XrXirValue map_captured_cell(struct XirCell *cell, uint32_t source) {
    if (!cell) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    const XirObject *object = (const XirObject *)cell;
    XrXirValue value = {(uint32_t)object->type, 0, (int64_t)(intptr_t)cell};
    const XrXirTypes *types = xr_xir_compile_type_arena_types(object->arena);
    XrXirType element = xr_xir_cell_element(types, object->type);
    const XrXirTypeNode *node = xr_xir_type_node(types, element);
    if (!xr_xir_value_argument(&value, object->arena, object->type) ||
        !node || node->kind != XR_XIR_TYPE_MAP || node->element != XR_XIR_STRING || node->result != XR_XIR_I64)
        xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    return value;
}
static XrtMap **map_ref_binding(XrtMapRef reference, uint32_t source) {
    if (reference.captured || reference.state.kind != XRT_MODULE_MAP_STRING_I64)
        xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    if (reference.cell) {
        if (reference.state.reference) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
        return reference.cell;
    }
    if (!reference.state.reference || !reference.state.flags || !reference.state.payload ||
        !reference.state.is_mutable || reference.state.size != sizeof(XrtMap *))
        xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    if ((*reference.state.flags & reference.state.required_flags) != reference.state.required_flags)
        xrt_module_uninitialized(source);
    return reference.state.payload;
}
XRAY_API XrtMapRef xrt_map_ref_captured(struct XirCell *cell, uint32_t source) {
    XrtMapRef reference = {.state={.kind=XRT_MODULE_MAP_STRING_I64}, .captured=cell};
    (void)map_captured_cell(cell, source);
    return reference;
}
XRAY_API XrtMap *xrt_map_ref_read(XrtMapRef reference, uint32_t source) {
    if (!reference.captured) return xrt_map_retain(*map_ref_binding(reference, source), source);
    if (reference.cell || reference.state.reference || reference.state.kind != XRT_MODULE_MAP_STRING_I64)
        xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    XrXirValue cell = map_captured_cell(reference.captured, source);
    XrtValueRoot *root = xrt_value_root_current(source);
    XR_CHECK(!root->pending.type && !root->pending.payload, "Captured Map reads own one empty pending result");
    XrXirValueStatus status = xr_xir_cell_authorized_read(&cell, NULL, &root->pending);
    if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source);
    return xrt_map_publish_pending(source);
}
XRAY_API void xrt_map_ref_write(XrtMapRef reference, const XrtMap *value, uint32_t source) {
    if (reference.captured) {
        if (reference.cell || reference.state.reference || reference.state.kind != XRT_MODULE_MAP_STRING_I64)
            xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
        XrXirValue cell = map_captured_cell(reference.captured, source);
        const XirObject *object = (const XirObject *)reference.captured;
        XrXirValueAdmission admission = {object->arena, object->domain, NULL, NULL, UINT64_MAX, UINT64_MAX};
        XrXirValue replacement = map_value(value);
        XrXirValueStatus status = xr_xir_cell_authorized_write(&cell, NULL, &replacement, &admission);
        if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source);
        return;
    }
    XrtMap **binding = map_ref_binding(reference, source);
    if (reference.cell) xrt_map_assign(binding, value, source);
    else xrt_module_require(xrt_module_replace(reference.state.reference, &value), source);
}
static bool map_captured_mutate(XrtMapRef reference, const XrtString *key,
    int64_t number, MapMutation operation, uint32_t source) {
    if (reference.cell || reference.state.reference || reference.state.kind != XRT_MODULE_MAP_STRING_I64)
        xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    XrXirValue cell = map_captured_cell(reference.captured, source);
    XrXirValue input = {XR_XIR_STRING, 0, (int64_t)(intptr_t)key};
    XrXirValue replacement = {XR_XIR_I64, 0, number}, retired = {0};
    bool removed = false;
    const XirObject *object = (const XirObject *)reference.captured;
    xr_xir_value_graph_begin();
    XrXirValueAdmission admission = {object->arena, object->domain, NULL, NULL, UINT64_MAX, UINT64_MAX};
    XrXirValuePlace place = {0};
    XrXirValueStatus status = xr_xir_cell_authorized_place(&cell, NULL, &admission, &place);
    if (status == XR_XIR_VALUE_OK) {
        XrtMap *current = NULL;
        memcpy(&current, place.payload, sizeof(current));
        XrXirValue value = map_value(current);
        const XrXirTypeArena *actual = xr_xir_value_arena(&value);
        uint64_t work = xir_type_arena_lookup_work(actual);
        if (work > admission.work || !xr_xir_domain_work(admission.domain, work)) status = XR_XIR_VALUE_LIMIT;
        else {
            admission.work -= work;
            if (!xir_type_arena_local_type(actual, admission.arena, place.type, &place.type))
                status = XR_XIR_VALUE_BAD_ARGUMENT;
            else admission.arena = actual;
        }
    }
    if (status == XR_XIR_VALUE_OK) {
        status = operation == MAP_SET ? xr_xir_map_set_deferred(&place, &input, &replacement, &admission, &retired) :
            operation == MAP_REMOVE ? xr_xir_hash_delete_deferred(&place, &input, &admission, &removed, &retired) :
            xr_xir_hash_clear_deferred(&place, &admission, &retired);
    }
    xr_xir_value_drop(&retired);
    xr_xir_value_graph_end();
    if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source);
    return removed;
}
XRAY_API void xrt_map_ref_string_i64_set(XrtMapRef reference, const XrtString *key,
    int64_t value, uint32_t source) {
    if (reference.captured) (void)map_captured_mutate(reference, key, value, MAP_SET, source);
    else (void)map_mutate(map_ref_binding(reference, source), reference.cell != NULL, key, value, MAP_SET, source);
}
XRAY_API bool xrt_map_ref_string_i64_remove(XrtMapRef reference, const XrtString *key, uint32_t source) {
    return reference.captured ? map_captured_mutate(reference, key, 0, MAP_REMOVE, source) :
        map_mutate(map_ref_binding(reference, source), reference.cell != NULL, key, 0, MAP_REMOVE, source);
}
XRAY_API void xrt_map_ref_clear(XrtMapRef reference, uint32_t source) {
    if (reference.captured) (void)map_captured_mutate(reference, NULL, 0, MAP_CLEAR, source);
    else (void)map_mutate(map_ref_binding(reference, source), reference.cell != NULL, NULL, 0, MAP_CLEAR, source);
}
