/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnet_provider.h - Shared VM-only network provider entry points
 */

#ifndef XR_IO_XNET_PROVIDER_H
#define XR_IO_XNET_PROVIDER_H

#include "../base/xdefs.h"
#include "../runtime/value/xvalue.h"
#include "../../include/xray_yieldable_abi.h"
#include "xtls_provider.h"

struct XrVMRuntime;

/* Run the existing non-blocking NetConn client handshake with a caller-owned
 * TLS context. Protocol modules choose ordering and the absolute deadline. */
XR_FUNC XrCFuncResult xr_net_tls_handshake_with_context(struct XrVMRuntime *X, XrValue *args,
                                                        int nargs, XrValue *result,
                                                        XrTlsContext *context);

/* Server-side counterpart: no hostname/SNI input, same absolute-deadline and
 * failure-closes-the-handle contract. */
XR_FUNC XrCFuncResult xr_net_tls_server_handshake_with_context(struct XrVMRuntime *X, XrValue *args,
                                                               int nargs, XrValue *result,
                                                               XrTlsContext *context);

#endif  // XR_IO_XNET_PROVIDER_H
