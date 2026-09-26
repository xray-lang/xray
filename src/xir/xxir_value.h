/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_value.h - Owned values and independently lived string allocation domains
 *
 * KEY CONCEPT:
 *   Explicit copy and drop preserve value semantics across execution boundaries.
 */
#ifndef XXIR_VALUE_H
#define XXIR_VALUE_H
#include "../base/xdefs.h"

#define XR_XIR_VALUE_ABI_VERSION 2u
#define XR_XIR_ARCH_X86_64 1u
typedef enum XrXirType { XR_XIR_UNIT, XR_XIR_BOOL, XR_XIR_I64, XR_XIR_STRING } XrXirType;
typedef struct XrXirValue {
    uint32_t type, reserved;
    int64_t payload;
} XrXirValue;
typedef enum XrXirValueStatus {
    XR_XIR_VALUE_OK, XR_XIR_VALUE_BAD_ARGUMENT, XR_XIR_VALUE_BAD_UTF8,
    XR_XIR_VALUE_OOM, XR_XIR_VALUE_LIMIT, XR_XIR_VALUE_REFCOUNT_LIMIT
} XrXirValueStatus;
typedef struct XrXirDomain XrXirDomain;
typedef struct XrXirDomainStats {
    uint64_t live_bytes, peak_bytes, allocations, frees, reallocations;
} XrXirDomainStats;

XR_FUNC XrXirValueStatus xr_xir_domain_new(uint64_t byte_limit, XrXirDomain **output);
XR_FUNC void xr_xir_domain_drop(XrXirDomain *domain);
XR_FUNC XrXirDomainStats xr_xir_domain_stats(XrXirDomain *domain);
XR_FUNC bool xr_xir_value_argument(const XrXirValue *value, XrXirType type);
/* Copy/new outputs must be canonical unit handles. Native pointers are trusted. */
XR_FUNC XrXirValueStatus xr_xir_value_copy(const XrXirValue *source, XrXirValue *output);
XR_FUNC void xr_xir_value_drop(XrXirValue *value);
XR_FUNC XrXirValueStatus xr_xir_string_new(XrXirDomain *domain, const char *bytes,
                                         size_t length, XrXirValue *output);
XR_FUNC XrXirValueStatus xr_xir_string_append(XrXirValue *destination, const XrXirValue *suffix);
XR_FUNC bool xr_xir_string_view(const XrXirValue *value, const char **bytes, size_t *length);
XR_FUNC bool xr_xir_string_runes(const XrXirValue *value, size_t *count);
XR_FUNC void xr_xir_string_slot_clear(void *frame, uint32_t offset);
XR_FUNC XrXirValueStatus xr_xir_string_slot_copy(void *frame, uint32_t offset, int64_t payload);
XR_FUNC XrXirValueStatus xr_xir_string_slot_concat(void *frame, uint32_t offset,
                                                  int64_t left, int64_t right);
#endif // XXIR_VALUE_H
