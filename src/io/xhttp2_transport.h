/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xhttp2_transport.h - Opaque TLS transport provider for http2.xr
 */

#ifndef XR_IO_XHTTP2_TRANSPORT_H
#define XR_IO_XHTTP2_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../base/xdefs.h"

struct XrVMRuntime;

XR_FUNC bool xr_http2_transport_supported(void);
XR_FUNC int64_t xr_http2_transport_connect(struct XrVMRuntime *isolate, const char *host, int port,
                                           int timeout_ms, const char *alpn, size_t alpn_length);
XR_FUNC int xr_http2_transport_send(struct XrVMRuntime *isolate, int64_t handle,
                                    const uint8_t *data, size_t length);
XR_FUNC int xr_http2_transport_recv(struct XrVMRuntime *isolate, int64_t handle, uint8_t *data,
                                    size_t length, int timeout_ms);
XR_FUNC void xr_http2_transport_close(struct XrVMRuntime *isolate, int64_t handle);

#endif  // XR_IO_XHTTP2_TRANSPORT_H
