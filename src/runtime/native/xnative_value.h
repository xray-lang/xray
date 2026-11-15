/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_value.h - One execution owner for stable managed values
 *
 * KEY CONCEPT:
 *   The outer caller owns this control object across setjmp. Guest owners live
 *   only in managed frames or actual State bindings, never a guest-count table.
 */
#ifndef XNATIVE_VALUE_H
#define XNATIVE_VALUE_H
#include "base/xdefs.h"
#include "shared/xr_value_status.h"

typedef struct XrtValueRoot {
    uint64_t byte_limit; /* Zero selects the ordinary unbounded policy. */
    struct XrtNativeValues *managed;
    bool draining;
} XrtValueRoot;
typedef struct XrtValueBinding { XrtValueRoot *root; size_t sources; } XrtValueBinding;

/* One synchronous thread binding is restored on every normal or failed exit. */
XR_FUNC XrtValueBinding xrt_value_root_bind(XrtValueBinding);
XR_FUNC XrtValueRoot *xrt_value_root_active(void);
XR_FUNC XrtValueRoot *xrt_value_root_current(uint32_t source_id);
XR_FUNC XR_NORET void xrt_value_failure(XrXirValueStatus, uint32_t source_id);
XR_FUNC void xrt_value_root_drain(XrtValueRoot *);
XR_FUNC void xrt_native_values_drain(XrtValueRoot *);
XR_FUNC bool xrt_native_values_prepared(const XrtValueRoot *);
static inline bool xrt_value_root_storage_empty(const XrtValueRoot *root) {
    return root && !root->draining;
}
static inline bool xrt_value_root_empty(const XrtValueRoot *root) {
    return xrt_value_root_storage_empty(root) && !root->managed;
}
#endif // XNATIVE_VALUE_H
