/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_cluster.h - Standalone AOT cluster runtime boundary
 */

#ifndef XRT_CLUSTER_H
#define XRT_CLUSTER_H

/* Resolved through the include path, not by source-tree layout: the installed
 * SDK flattens these headers and a relative climb would not exist there. */
#include "xray_value_abi.h"

#include <stdint.h>

#ifndef XR_FUNC
#define XR_FUNC extern
#endif

XR_FUNC XrValue xrt_cluster_start(const char *name, int64_t name_len, XrValue port,
                                  const char *secret, int64_t secret_len, XrValue tls_enabled,
                                  const char *ca_file, int64_t ca_file_len, const char *cert_file,
                                  int64_t cert_file_len, const char *key_file, int64_t key_file_len,
                                  XrValue insecure, XrValue heartbeat_interval_ms,
                                  XrValue heartbeat_timeout_ms, XrValue max_missed_heartbeats,
                                  XrValue heartbeat_tick_ms, XrValue phi_min_samples,
                                  XrValue phi_threshold, XrValue topic_delivery_fanout_max,
                                  XrValue tombstone_retention_ms);
XR_FUNC XrValue xrt_cluster_join(const char *host, int64_t host_len, XrValue port);
XR_FUNC XrValue xrt_cluster_stop(void);
XR_FUNC int64_t xrt_cluster_publish_local(const char *topic, int64_t topic_len, XrValue envelope);
XR_FUNC int64_t xrt_cluster_publish_remote(const char *topic, int64_t topic_len, XrValue envelope,
                                           XrValue hop_limit);
XR_FUNC XrValue xrt_cluster_listen(const char *pattern, int64_t pattern_len, XrValue capacity);

#endif  // XRT_CLUSTER_H
