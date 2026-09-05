/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xa_suspend_point.h - Exact compiler-owned source suspend-point facts
 */

#ifndef XA_SUSPEND_POINT_H
#define XA_SUSPEND_POINT_H

#include <stdint.h>

typedef enum XaSuspendPointKind {
    XA_SUSPEND_POINT_NONE = 0,
    XA_SUSPEND_POINT_COOPERATIVE_YIELD = 1,
} XaSuspendPointKind;

typedef struct XaSuspendPointFact {
    uint16_t kind; /* XaSuspendPointKind */
    uint8_t may_suspend;
    uint8_t complete;
} XaSuspendPointFact;

typedef struct XaNodeSuspendPointEntry {
    uint32_t node_id;
    XaSuspendPointFact fact;
} XaNodeSuspendPointEntry;

#endif  // XA_SUSPEND_POINT_H
