/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_hash_position.inc.c - Bounded ordinal reads preserve ownership and order
 */
static XrXirValueStatus hash_unpack(const XirHash *map, size_t index, bool value,
    XrXirValueAdmission *admission, XrXirValue *output) {
    XrXirValueAdmission local = *admission;
    local.arena = map->object.arena;
    XrXirValueStatus status = storage_unpack(hash_span(map, index, value), &local, output);
    return storage_admission_end(admission, &local, status);
}
static XrXirValueStatus hash_position(const XrXirValue *container, int64_t index, bool value,
    XrXirValueAdmission *admission, XrXirValue *output, XrXirFaultDetail *fault) {
    if (!unit_value(output) || !fault || !hash_admission_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    *fault = (XrXirFaultDetail){0}; xr_xir_value_graph_begin();
    XirHash *map = NULL;
    XrXirValueStatus status = hash_admit_received(container, value ? XR_XIR_TYPE_MAP : 0, admission, &map);
    if (status == XR_XIR_VALUE_OK && (index < 0 || (uint64_t)index >= map->length)) {
        *fault = (XrXirFaultDetail){XR_XIR_PANIC_BOUNDS, 0, index, (int64_t)map->length}; status = XR_XIR_VALUE_BOUNDS;
    }
    if (status == XR_XIR_VALUE_OK) status = hash_charge(admission, 1);
    if (status == XR_XIR_VALUE_OK && (!value || map->value_type != XR_XIR_UNIT))
        status = hash_unpack(map, (size_t)index, value, admission, output);
    xr_xir_value_graph_end(); return status;
}
XR_FUNC XrXirValueStatus xr_xir_hash_key_at(const XrXirValue *container, int64_t index,
    XrXirValueAdmission *admission, XrXirValue *output, XrXirFaultDetail *fault) {
    return hash_position(container, index, false, admission, output, fault);
}
XR_FUNC XrXirValueStatus xr_xir_map_value_at(const XrXirValue *map, int64_t index,
    XrXirValueAdmission *admission, XrXirValue *output, XrXirFaultDetail *fault) {
    return hash_position(map, index, true, admission, output, fault);
}
