/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcluster_blocking.h - Blocking socket provider for standalone AOT cluster
 */

#ifndef XR_IO_CLUSTER_BLOCKING_H
#define XR_IO_CLUSTER_BLOCKING_H

#include "xcluster_wire.h"
#include "../base/xdefs.h"
#include "../os/os_net.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

XR_FUNC int xr_cluster_blocking_wait(xr_socket_t socket, bool read_ready, int timeout_ms);
XR_FUNC bool xr_cluster_blocking_write_all(xr_socket_t socket, const uint8_t *data, size_t length,
                                           int timeout_ms);
XR_FUNC bool xr_cluster_blocking_read_frame(xr_socket_t socket, uint8_t *type, uint8_t **payload,
                                            uint32_t *payload_length, int timeout_ms);
XR_FUNC bool xr_cluster_blocking_server_handshake(xr_socket_t socket, const char *self_name,
                                                  const char *secret, uint32_t flags,
                                                  char peer_name[XR_NODE_NAME_MAX + 1]);
XR_FUNC bool xr_cluster_blocking_client_handshake(xr_socket_t socket, const char *self_name,
                                                  const char *secret, uint32_t flags,
                                                  char peer_name[XR_NODE_NAME_MAX + 1]);

#endif /* XR_IO_CLUSTER_BLOCKING_H */
