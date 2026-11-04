/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_signal_frontier_internal.h - Read-only interruption eligibility
 *
 * KEY CONCEPT:
 *   An idle driver frontier is not permission to execute a callback. Actual
 *   frame loans and sealed promises can only defer that separate operation.
 */
#ifndef XXIR_SIGNAL_FRONTIER_INTERNAL_H
#define XXIR_SIGNAL_FRONTIER_INTERNAL_H
#include "xxir_call_internal.h"

/* Invalid owners and busy drivers leave output unchanged. READY publishes an
 * observation only; it does not consume waits, values, loans or pending faults. */
XR_FUNC XrXirCallStatus xir_call_signal_frontier(const XrXirCall *call,bool *eligible);
/* Called synchronously with an actual owned frame's metadata. Unknown raw
 * tables have no sealed promise information and are never eligible. */
XR_FUNC bool xir_instance_signal_frame(const void *instance,const XrXirCall *call,
    const XrXirValueAdmission *admission,const XrXirCallMetadata *metadata,
    uint32_t entry,bool *no_suspend);
#endif // XXIR_SIGNAL_FRONTIER_INTERNAL_H
