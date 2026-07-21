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

/* Hosted AOT does not yet carry the native HTTP/2 connection-pool/data-plane
 * implementation. The script layer checks this capability before calling the
 * request helper and raises HttpError.UnsupportedVersion(Http2). */
static inline XrValue xrt_http_h2_supported(void) {
    return XR_FALSE_VAL;
}

static inline XrValue xrt_http_h2_request_unavailable(XrValue url, XrValue method,
                                                      XrValue header_names, XrValue header_values,
                                                      XrValue body, XrValue timeout_ms) {
    (void) url;
    (void) method;
    (void) header_names;
    (void) header_values;
    (void) body;
    (void) timeout_ms;
    return XR_NULL_VAL;
}

#endif  // XRT_HTTP_H
