/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_string_instance_probe.h - Actual Instance retirement observations for string tests
 *
 * KEY CONCEPT:
 *   Observe the production object at retirement, without duplicating its layout.
 */
#ifndef XIR_STRING_INSTANCE_PROBE_H
#define XIR_STRING_INSTANCE_PROBE_H
#include "xir/xxir_program.h"
typedef struct XirStringRetirement {
    XrXirDomain *domain;
    XrXirDomainStats baseline;
    XrXirCallAccounting accounting[2];
} XirStringRetirement;
XR_FUNC XrXirDomain *xr_string_instance_domain(XrXirInstance *instance);
XR_FUNC XrXirCallStatus xr_string_instance_completion(XrXirInstance *instance);
XR_FUNC void xr_string_instance_output_busy(XrXirInstance *instance, bool cancel);
XR_FUNC XirStringRetirement xr_string_instance_retire(XrXirInstance *instance,
    XrXirCallStatus expected, uint64_t poll_limit);
#endif // XIR_STRING_INSTANCE_PROBE_H
