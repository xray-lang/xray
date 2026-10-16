/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_hash_value.inc.c - Budgeted typed Map and Set construction and lookup
 */
static XrXirValueStatus hash_new(XrXirType type, const XrXirValue *keys,
    const XrXirValue *values, size_t count, XrXirValueAdmission *admission, XrXirValue *output) {
    if (!unit_value(output) || (count && !keys)) return XR_XIR_VALUE_BAD_ARGUMENT;
    HashLayout layout = {0};
    XrXirValueStatus status = hash_layout(admission->arena, type, &layout);
    if (status != XR_XIR_VALUE_OK) return status;
    const XrXirTypeNode *node = xr_xir_type_node(xr_xir_compile_type_arena_types(admission->arena), type);
    if ((node->kind == XR_XIR_TYPE_MAP && count && !values) ||
        (node->kind == XR_XIR_TYPE_SET && values)) return XR_XIR_VALUE_BAD_ARGUMENT;
    size_t capacity = 0;
    if (!hash_capacity(count, layout.stride, &capacity)) return XR_XIR_VALUE_LIMIT;
    XirHash *map = NULL;
    status = hash_allocate(type, capacity, admission, &map);
    if (status != XR_XIR_VALUE_OK) return status;
    XrXirValue result = hash_value(map);
    XrXirValuePlace place = {type, &result.payload};
    for (size_t i = 0; i < count; ++i) {
        status = hash_upsert(&place, &keys[i], values ? &values[i] : NULL, admission);
        if (status != XR_XIR_VALUE_OK) { xr_xir_value_drop(&result); return status; }
    }
    *output = result; return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_map_new(XrXirType type, const XrXirValue *keys,
    const XrXirValue *values, size_t count, XrXirValueAdmission *admission, XrXirValue *output) {
    if (!hash_admission_current(admission) ||
        !xr_xir_type_is_map(xr_xir_compile_type_arena_types(admission->arena), type))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus status = hash_new(type, keys, values, count, admission, output);
    xr_xir_value_graph_end(); return status;
}
XR_FUNC XrXirValueStatus xr_xir_set_new(XrXirType type, const XrXirValue *elements,
    size_t count, XrXirValueAdmission *admission, XrXirValue *output) {
    if (!hash_admission_current(admission) ||
        !xr_xir_type_is_set(xr_xir_compile_type_arena_types(admission->arena), type))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus status = hash_new(type, elements, NULL, count, admission, output);
    xr_xir_value_graph_end(); return status;
}
XR_FUNC XrXirValueStatus xr_xir_hash_len(const XrXirValue *container,
    XrXirValueAdmission *admission, int64_t *output) {
    if (!output || !hash_admission_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueAdmission local = {0};
    XrXirValueStatus status = storage_admission_begin(container, admission, &local);
    if (status != XR_XIR_VALUE_OK) return status;
    xr_xir_value_graph_begin();
    XirHash *map = NULL;
    status = hash_admit(container, 0, &local, &map);
    if (status == XR_XIR_VALUE_OK) *output = (int64_t)map->length;
    xr_xir_value_graph_end(); return storage_admission_end(admission, &local, status);
}
XR_FUNC XrXirValueStatus xr_xir_map_get(const XrXirValue *value, const XrXirValue *key,
    XrXirValueAdmission *admission, XrXirValue *output, bool *found) {
    if (!unit_value(output) || !found || !hash_admission_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueAdmission local = {0};
    XrXirValueStatus status = storage_admission_begin(value, admission, &local);
    if (status != XR_XIR_VALUE_OK) return status;
    xr_xir_value_graph_begin();
    XirHash *map = NULL; HashProbe probe = {0};
    status = hash_admit(value, XR_XIR_TYPE_MAP, &local, &map);
    if (status == XR_XIR_VALUE_OK) status = hash_probe(map, key, &local, &probe);
    if (status == XR_XIR_VALUE_OK && probe.found && map->value_type != XR_XIR_UNIT)
        status = storage_unpack(hash_span(map, probe.entry, true), &local, output);
    if (status == XR_XIR_VALUE_OK) *found = probe.found;
    xr_xir_value_graph_end(); return storage_admission_end(admission, &local, status);
}
static XrXirValueStatus hash_contains(const XrXirValue *value, const XrXirValue *key,
    uint32_t kind, XrXirValueAdmission *admission, bool *output) {
    if (!output || !hash_admission_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueAdmission local = {0};
    XrXirValueStatus status = storage_admission_begin(value, admission, &local);
    if (status != XR_XIR_VALUE_OK) return status;
    xr_xir_value_graph_begin();
    XirHash *map = NULL; HashProbe probe = {0};
    status = hash_admit(value, kind, &local, &map);
    if (status == XR_XIR_VALUE_OK) status = hash_probe(map, key, &local, &probe);
    if (status == XR_XIR_VALUE_OK) *output = probe.found;
    xr_xir_value_graph_end(); return storage_admission_end(admission, &local, status);
}
XR_FUNC XrXirValueStatus xr_xir_map_contains_key(const XrXirValue *map, const XrXirValue *key,
    XrXirValueAdmission *admission, bool *output) {
    return hash_contains(map, key, XR_XIR_TYPE_MAP, admission, output);
}
XR_FUNC XrXirValueStatus xr_xir_set_contains(const XrXirValue *set, const XrXirValue *element,
    XrXirValueAdmission *admission, bool *output) {
    return hash_contains(set, element, XR_XIR_TYPE_SET, admission, output);
}
