/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcoro_snapshot.h - Best-effort runtime coroutine snapshots
 *
 * KEY CONCEPT:
 *   Runtime diagnostics observe real scheduler queues instead of a legacy
 *   isolate-level ready list. The snapshot is intentionally best-effort:
 *   workers may move coroutines while diagnostics are reading.
 */

#ifndef XCORO_SNAPSHOT_H
#define XCORO_SNAPSHOT_H

#include "../base/xdefs.h"
#include "xcoroutine.h"

typedef struct XrRuntime XrRuntime;

typedef struct XrCoroSnapshotEntry {
    XrCoroutine *coro;
    const char *state;
} XrCoroSnapshotEntry;

XR_FUNC int xr_runtime_collect_coros(XrRuntime *runtime, XrCoroSnapshotEntry *out, int max_out);

#endif /* XCORO_SNAPSHOT_H */
