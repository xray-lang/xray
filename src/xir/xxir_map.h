/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_map.h - Canonical typed hash-container identities
 *
 * KEY CONCEPT:
 *   Map key/value and Set element identities belong to one verified type pool.
 *   Shape classification grants no Hashable proof or declaration authority.
 */
#ifndef XXIR_MAP_H
#define XXIR_MAP_H
#include "xxir.h"
#include "xxir_value_place.h"
XR_FUNC bool xr_xir_type_is_map(const XrXirTypes *types, XrXirType type);
XR_FUNC bool xr_xir_type_is_set(const XrXirTypes *types, XrXirType type);
XR_FUNC XrXirType xr_xir_map_key(const XrXirTypes *types, XrXirType type);
XR_FUNC XrXirType xr_xir_map_element(const XrXirTypes *types, XrXirType type);
XR_FUNC XrXirType xr_xir_set_element(const XrXirTypes *types, XrXirType type);
/* Outputs start as canonical Unit. Duplicate input keys keep their first key
 * and replace their value. Every operation uses the receiving finite owner. */
XR_FUNC XrXirValueStatus xr_xir_map_new(XrXirType type, const XrXirValue *keys,
    const XrXirValue *values, size_t count, XrXirValueAdmission *admission, XrXirValue *output);
XR_FUNC XrXirValueStatus xr_xir_set_new(XrXirType type, const XrXirValue *elements,
    size_t count, XrXirValueAdmission *admission, XrXirValue *output);
XR_FUNC XrXirValueStatus xr_xir_hash_len(const XrXirValue *container,
    XrXirValueAdmission *admission, int64_t *output);
/* Missing keys return found=false and leave the Unit output empty. Failure
 * preserves both outputs. A present Unit value is distinguished by found. */
XR_FUNC XrXirValueStatus xr_xir_map_get(const XrXirValue *map, const XrXirValue *key,
    XrXirValueAdmission *admission, XrXirValue *output, bool *found);
XR_FUNC XrXirValueStatus xr_xir_set_contains(const XrXirValue *set, const XrXirValue *element,
    XrXirValueAdmission *admission, bool *output);
/* Places require a caller-pinned writable root and authenticated whole loan.
 * Shared roots separate; failed preparation never publishes partial storage. */
XR_FUNC XrXirValueStatus xr_xir_map_set(const XrXirValuePlace *place, const XrXirValue *key,
    const XrXirValue *value, XrXirValueAdmission *admission);
XR_FUNC XrXirValueStatus xr_xir_set_add(const XrXirValuePlace *place, const XrXirValue *element,
    XrXirValueAdmission *admission);
XR_FUNC XrXirValueStatus xr_xir_hash_delete(const XrXirValuePlace *place, const XrXirValue *key,
    XrXirValueAdmission *admission, bool *removed);
XR_FUNC XrXirValueStatus xr_xir_hash_clear(const XrXirValuePlace *place,
    XrXirValueAdmission *admission);
#endif // XXIR_MAP_H
