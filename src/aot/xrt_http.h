/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_http.h - AOT capability boundary for the pure-Xray HTTP control plane.
 */

#ifndef XRT_HTTP_H
#define XRT_HTTP_H

#include "xrt_value.h"

/* Hosted AOT currently has no TLS transport provider. Keep the build fact so
 * source can query availability, but do not pretend that unusable connect,
 * send, receive, or close implementations are transport leaves. Programs
 * that reach those leaves fail planning until AOT gains a real TLS provider. */
static inline XrValue xrt_http_h2_supported(void) {
    return XR_FALSE_VAL;
}

#endif  // XRT_HTTP_H
