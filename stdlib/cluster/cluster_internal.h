/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * cluster_internal.h - Private cluster lifecycle provider state
 *
 * Xray owns every cluster resource and policy value. Native state is restricted
 * to the isolate lifecycle slot used to invoke source stop during teardown.
 */

#ifndef XR_CLUSTER_INTERNAL_H
#define XR_CLUSTER_INTERNAL_H

#include "../../src/base/xchecks.h"
#include "../../src/base/xmalloc.h"
#include "../../src/module/xmodule.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

struct XrVMRuntime;
typedef struct XrCluster XrCluster;

typedef struct XrCluster {
    _Atomic(uint32_t) ref_count;
    struct XrVMRuntime *isolate;
    _Atomic(bool) running;

} XrCluster;

static inline void cluster_runtime_retain(void *provider) {
    XrCluster *cluster = (XrCluster *) provider;
    if (cluster)
        atomic_fetch_add(&cluster->ref_count, 1);
}

/* Slot lookup and the strong-reference increment form one critical section. */
#define XR_CLUSTER_RUNTIME_ACQUIRE(isolate, out_cluster)                                           \
    do {                                                                                           \
        (out_cluster) = (XrCluster *) xr_isolate_provider_acquire((isolate), &(isolate)->cluster,  \
                                                                  cluster_runtime_retain);         \
    } while (0)

static inline void cluster_runtime_release(XrCluster *cluster) {
    if (!cluster)
        return;
    uint32_t previous = atomic_fetch_sub(&cluster->ref_count, 1);
    XR_DCHECK(previous > 0, "cluster reference underflow");
    if (previous != 1)
        return;

    xr_free(cluster);
}

#endif // XR_CLUSTER_INTERNAL_H
