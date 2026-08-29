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

/* Hosted AOT has never carried HTTP/2: it has no TLS transport to hand the
 * protocol, so every leaf below has always answered "unavailable" and the
 * stubs here only re-spell that same fail-closed contract for the four
 * transport leaves that replaced the old single request helper. Moving the
 * protocol into stdlib/http2/http2.xr did not take anything away from AOT.
 *
 * The script layer checks xrt_http_h2_supported() first and raises
 * Http2Error.TlsUnavailable, so the remaining stubs are only reached by a
 * caller that ignored the capability answer. */
static inline XrValue xrt_http_h2_supported(void) {
    return XR_FALSE_VAL;
}

/* __connect(host, port, timeoutMs) -> i64; -1 is "no connection". */
static inline XrValue xrt_http_h2_connect_unavailable(XrValue host, XrValue port,
                                                      XrValue timeout_ms) {
    (void) host;
    (void) port;
    (void) timeout_ms;
    return XR_FROM_INT(-1);
}

/* __send(handle, data) -> bool */
static inline XrValue xrt_http_h2_send_unavailable(XrValue handle, XrValue data) {
    (void) handle;
    (void) data;
    return XR_FALSE_VAL;
}

/* __recv(handle, maxBytes, timeoutMs) -> Array<u8>?; null is "no bytes". */
static inline XrValue xrt_http_h2_recv_unavailable(XrValue handle, XrValue max_bytes,
                                                   XrValue timeout_ms) {
    (void) handle;
    (void) max_bytes;
    (void) timeout_ms;
    return XR_NULL_VAL;
}

/* __close(handle) -> bool */
static inline XrValue xrt_http_h2_close_unavailable(XrValue handle) {
    (void) handle;
    return XR_FALSE_VAL;
}

#endif  // XRT_HTTP_H
