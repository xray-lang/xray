/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_hash_iteration.h - Owned ordinal reads from insertion-ordered snapshots
 */
#ifndef XXIR_HASH_ITERATION_H
#define XXIR_HASH_ITERATION_H
#include "xxir_map.h"
#include "xxir_fault.h"
/* Ordinals address compact insertion order, never the public Map key space.
 * Every result is an owned copy; failures leave the Unit output unchanged. */
XR_FUNC XrXirValueStatus xr_xir_hash_key_at(const XrXirValue *container, int64_t index,
    XrXirValueAdmission *admission, XrXirValue *output, XrXirFaultDetail *fault);
XR_FUNC XrXirValueStatus xr_xir_map_value_at(const XrXirValue *map, int64_t index,
    XrXirValueAdmission *admission, XrXirValue *output, XrXirFaultDetail *fault);
#endif // XXIR_HASH_ITERATION_H
