/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * cluster.c - Cluster isolate-lifecycle bindings
 *
 * Peer identity, framing, queues, health facts, TLS resources and transport
 * coroutines are ordinary state in cluster.xr. This provider owns only the
 * isolate-local lifetime slot that is detached during teardown.
 */

#include "cluster.h"
#include "cluster_internal.h"
#include "../../stdlib/common.h"
#include "../../src/runtime/xisolate_internal.h"
#include "../../src/runtime/value/xvalue.h"
#include "../../src/vm/xvm.h"

static XrValue cluster_stop_fn(XrVMRuntime *X, XrValue *args, int argc);

/* Source validates the complete configuration and owns all resources. This
 * leaf only publishes the isolate lifecycle slot. */
static XrValue cluster_start_primitive(XrVMRuntime *X, XrValue *args, int argc) {
    (void) args;
    if (argc != 0)
        return xr_bool(false);
    XrCluster *cluster = (XrCluster *) xr_calloc(1, sizeof(*cluster));
    if (!cluster)
        return xr_bool(false);
    atomic_store(&cluster->ref_count, 1);
    cluster->isolate = X;

    atomic_store(&cluster->running, true);
    if (!xr_isolate_provider_publish(X, &X->cluster, cluster, cluster_stop_fn)) {
        atomic_store(&cluster->running, false);
        cluster_runtime_release(cluster);
        return xr_bool(false);
    }
    return xr_bool(true);
}

static XrValue cluster_stop_fn(XrVMRuntime *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrCluster *cluster =
        (XrCluster *) xr_isolate_provider_detach(X, &X->cluster, cluster_stop_fn);
    if (!cluster)
        return xr_null();
    atomic_store(&cluster->running, false);
    cluster_runtime_release(cluster);
    return xr_null();
}

#define XR_STDLIB_VM_BIND_MODULE_CLUSTER 1
#include "../../src/stdlib/xstdlib_vm_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_MODULE_CLUSTER
