/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */

#ifndef XRAY_IO_CLUSTER_DISCOVERY_PROVIDER_H
#define XRAY_IO_CLUSTER_DISCOVERY_PROVIDER_H

#include "../base/xdefs.h"
#include "../runtime/value/xvalue.h"

#ifdef __cplusplus
extern "C" {
#endif

struct XrVMRuntime;

/* Open the multicast UDP handle used by cluster.xr's discovery loop. */
XR_FUNC XrValue xr_cluster_discovery_socket_open(struct XrVMRuntime *X, XrValue *args, int argc);

#ifdef __cplusplus
}
#endif

#endif /* XRAY_IO_CLUSTER_DISCOVERY_PROVIDER_H */
