/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_tuple.h - Immutable ordered values with independent field ownership
 */
#ifndef XXIR_TUPLE_H
#define XXIR_TUPLE_H
#include "xxir_value.h"
/* Fields are the complete ordered vector, including canonical Unit values.
 * Publication retains its arena, allocation domain and each owned field.
 * Failure preserves every input and the canonical Unit output. */
XR_FUNC XrXirValueStatus xr_xir_tuple_new(XrXirType type, const XrXirValue *fields,
    uint32_t count, XrXirValueAdmission *admission, XrXirValue *output);
/* Projection copies data ownership and grants no execution permission. */
XR_FUNC XrXirValueStatus xr_xir_tuple_get(const XrXirValue *value, uint32_t field,
    XrXirValue *output);
#endif // XXIR_TUPLE_H
