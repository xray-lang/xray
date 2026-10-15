/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_class.h - Owned class identity operations
 *
 * KEY CONCEPT:
 *   Identity handles pin their field storage, type arena and physical accounting domain.
 */
#ifndef XXIR_CLASS_H
#define XXIR_CLASS_H
#include "xxir_value.h"
/* Trusted callers already hold checked construction/member permission.
 * GET reads retained data only and never recovers executable authority.
 * An authenticated received identity selects the actual owner layout; it
 * grants no Domain, callable, loan or publication permission. Work and scratch
 * remain bounded. Leaf copies need no allocation domain; inline snapshots
 * require an explicit live domain,
 * which owns new materialized storage but never takes over existing leaves. */
XR_FUNC XrXirValueStatus xr_xir_class_new(XrXirType type, const XrXirValue *fields,
    uint32_t count, XrXirValueAdmission *admission, XrXirValue *output);
XR_FUNC XrXirValueStatus xr_xir_class_get(const XrXirValue *receiver, uint32_t field,
    XrXirValueAdmission *admission, XrXirValue *output);
XR_FUNC XrXirValueStatus xr_xir_class_set(const XrXirValue *receiver, uint32_t field,
    const XrXirValue *replacement, XrXirValueAdmission *admission);
#endif // XXIR_CLASS_H
