/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_map_contains_value.inc.c - Bounded value membership under ordinary equality
 *
 * KEY CONCEPT:
 *   Key equivalence cannot replace IEEE value equality for stored values.
 */
XR_FUNC XrXirValueStatus xr_xir_map_contains_value(const XrXirValue *container,
    const XrXirValue *value, XrXirValueAdmission *admission, bool *output) {
    if (!output || !value || !hash_admission_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XirHash *map = NULL;
    XrXirValueStatus status = hash_admit_received(container, XR_XIR_TYPE_MAP, admission, &map);
    XrXirValueAdmission local = *admission;
    if (status == XR_XIR_VALUE_OK) local.arena = map->object.arena;
    if (status == XR_XIR_VALUE_OK) status = equal_closed_type(map->value_type, &local);
    if (status == XR_XIR_VALUE_OK) status = xr_xir_value_admit(value, map->value_type, &local);
    bool found = false;
    for (size_t i = 0; status == XR_XIR_VALUE_OK && !found && i < map->length; ++i) {
        XrXirValue item = {0};
        status = hash_charge(&local, 1);
        if (status == XR_XIR_VALUE_OK) status = storage_unpack(hash_span(map, i, true), &local, &item);
        if (status == XR_XIR_VALUE_OK) status = xr_xir_value_equal(&item, value, map->value_type, &local, &found);
        /* The temporary owns its unpacked leaf until comparison has finished. */
        xr_xir_value_drop(&item);
    }
    if (status == XR_XIR_VALUE_OK) *output = found;
    xr_xir_value_graph_end(); return storage_admission_end(admission, &local, status);
}
