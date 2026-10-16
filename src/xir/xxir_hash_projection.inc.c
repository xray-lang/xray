/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_hash_projection.inc.c - Ordered independent hash-container snapshots
 */
typedef enum HashProjection { HASH_KEYS, HASH_VALUES, HASH_ENTRIES } HashProjection;
static bool hash_projection_type(const XirHash *map, XrXirType array_type,
    HashProjection projection, const XrXirTypeArena *arena, XrXirType *element) {
    const XrXirTypes *types = xr_xir_compile_type_arena_types(arena);
    bool is_map = map->object.kind == XR_XIR_TYPE_MAP;
    if ((!is_map && projection != HASH_VALUES) || !xr_xir_type_is_array(types, array_type)) return false;
    *element = xr_xir_array_element(types, array_type);
    if (projection != HASH_ENTRIES)
        return xir_type_arena_same_type(arena, *element, map->object.arena,
            projection == HASH_KEYS || !is_map ? map->key_type : map->value_type);
    const XrXirTypeNode *tuple = xr_xir_tuple_signature(types, *element);
    return tuple && tuple->parameter_count == 2 &&
        !tuple->parameters[0].mode && !tuple->parameters[1].mode &&
        xir_type_arena_same_type(arena, tuple->parameters[0].type, map->object.arena, map->key_type) &&
        xir_type_arena_same_type(arena, tuple->parameters[1].type, map->object.arena, map->value_type);
}
static XrXirValueStatus hash_projection_item(XirHash *map, size_t index, XrXirType element,
    HashProjection projection, XrXirValueAdmission *admission, XrXirValue *output) {
    bool is_map = xr_xir_type_is_map(xr_xir_compile_type_arena_types(map->object.arena), map->object.type);
    if (projection != HASH_ENTRIES)
        return hash_unpack(map, index, projection == HASH_VALUES && is_map, admission, output);
    XrXirValue fields[2] = {{0}};
    XrXirValueStatus status = hash_unpack(map, index, false, admission, fields);
    if (status == XR_XIR_VALUE_OK) status = hash_unpack(map, index, true, admission, fields + 1);
    if (status == XR_XIR_VALUE_OK) status = xr_xir_tuple_new(element, fields, 2, admission, output);
    xr_xir_value_drop(fields); xr_xir_value_drop(fields + 1);
    return status;
}
static XrXirValueStatus hash_projection(const XrXirValue *container, XrXirType array_type,
    HashProjection projection, XrXirValueAdmission *admission, XrXirValue *output) {
    if (!unit_value(output) || !hash_admission_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XirHash *map = NULL; XrXirType element = XR_XIR_UNIT;
    XrXirValueStatus status = hash_admit_received(container, projection == HASH_VALUES ? 0 : XR_XIR_TYPE_MAP, admission, &map);
    if (status == XR_XIR_VALUE_OK) status = hash_charge(admission, 3);
    if (status == XR_XIR_VALUE_OK && !hash_projection_type(map, array_type, projection,
        admission->arena, &element))
        status = XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValue result = {0};
    if (status == XR_XIR_VALUE_OK)
        status = xr_xir_array_with_capacity(array_type, (int64_t)map->length, admission, &result);
    XirArray *array = status == XR_XIR_VALUE_OK ? (XirArray *)object_pointer(&result) : NULL;
    StoragePack pack = {0}; bool packing = false;
    if (status == XR_XIR_VALUE_OK) {
        status = storage_pack_begin(admission, element, &pack);
        packing = status == XR_XIR_VALUE_OK;
    }
    for (size_t i = 0; status == XR_XIR_VALUE_OK && i < map->length; ++i) {
        XrXirValue item = {0}; status = hash_charge(admission, 1);
        if (status == XR_XIR_VALUE_OK) status = hash_projection_item(map, i, element, projection, admission, &item);
        /* The private destination has no external alias. Pack each admitted
         * item once instead of repeatedly walking its growing prefix. */
        if (status == XR_XIR_VALUE_OK) status = storage_pack_value(&pack, &item, array_slot(array, i));
        if (status == XR_XIR_VALUE_OK) ++array->length;
        xr_xir_value_drop(&item);
    }
    if (packing) storage_pack_end(&pack);
    if (status == XR_XIR_VALUE_OK) { *output = result; result = (XrXirValue){0}; }
    xr_xir_value_drop(&result); xr_xir_value_graph_end(); return status;
}
XR_FUNC XrXirValueStatus xr_xir_map_keys(const XrXirValue *map, XrXirType array_type,
    XrXirValueAdmission *admission, XrXirValue *output) {
    return hash_projection(map, array_type, HASH_KEYS, admission, output);
}
XR_FUNC XrXirValueStatus xr_xir_hash_values(const XrXirValue *container, XrXirType array_type,
    XrXirValueAdmission *admission, XrXirValue *output) {
    return hash_projection(container, array_type, HASH_VALUES, admission, output);
}
XR_FUNC XrXirValueStatus xr_xir_map_entries(const XrXirValue *map, XrXirType array_type,
    XrXirValueAdmission *admission, XrXirValue *output) {
    return hash_projection(map, array_type, HASH_ENTRIES, admission, output);
}
