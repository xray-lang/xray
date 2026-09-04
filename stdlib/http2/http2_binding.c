/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * http2_binding.c - HTTP/2 transport leaves
 *
 * KEY CONCEPT:
 *   Four leaves that move opaque bytes over one TLS connection negotiated
 *   with a caller-selected ALPN protocol, plus a build-capability probe. None
 *   of them knows what a frame is: HPACK, framing, the stream state machine
 *   and flow control all live in stdlib/http2/http2.xr. What is left here is
 *   the part Xray cannot state -- a socket, a TLS session, and the protocol
 *   the peer agreed to speak over it.
 *
 *   Connections are addressed by an integer handle rather than a pointer, so
 *   no address ever crosses into Xray. A handle carries a generation counter,
 *   which is what makes a stale handle fail cleanly instead of aliasing onto
 *   whatever connection later reused its slot.
 */

#include "../../stdlib/common.h"
#include "../../src/io/xhttp2_transport.h"
#include "../../src/runtime/mem/xheap.h"
#include "../../src/runtime/object/xarray.h"
#include "../../src/runtime/object/xstring.h"
#include "../../src/vm/xvm.h"

/* ========== Leaves ========== */

static XrValue h2_supported(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    (void) args;
    if (argc != 0)
        return xr_bool(false);
    return xr_bool(xr_http2_transport_supported());
}

/* http2.__connect(host, port, timeoutMs, alpn) -> i64. Negative values are
 * raw provider outcomes that http2.xr maps to typed public errors:
 * -1 transport failure, -2 TLS unavailable, -3 ALPN mismatch. */
static XrCFuncResult h2_connect(XrVMRuntime *X, XrValue *args, int nargs, XrValue *result) {
    *result = xr_int(-1);
    if (nargs != 4 || !XR_IS_STRING(args[0]) || !XR_IS_INT(args[1]) || !XR_IS_INT(args[2]) ||
        !XR_IS_STRING(args[3]))
        return XR_CFUNC_DONE;

    int64_t port = XR_TO_INT(args[1]);
    int64_t timeout_value = XR_TO_INT(args[2]);
    if (port < 1 || port > 65535 || timeout_value <= 0 || timeout_value > INT32_MAX)
        return XR_CFUNC_DONE;
    const char *host = XR_STRING_CHARS(XR_TO_STRING(args[0]));
    XrString *requested_alpn = XR_TO_STRING(args[3]);
    if (!host || requested_alpn->length == 0 || requested_alpn->length > UINT8_MAX)
        return XR_CFUNC_DONE;

    *result =
        xr_int(xr_http2_transport_connect(X, host, (int) port, (int) timeout_value,
                                          XR_STRING_CHARS(requested_alpn), requested_alpn->length));
    return XR_CFUNC_DONE;
}

/* http2.__send(handle, data, offset) -> i64; performs one TLS write and
 * answers the accepted byte count, or a negative value on failure. */
static XrCFuncResult h2_send(XrVMRuntime *X, XrValue *args, int nargs, XrValue *result) {
    *result = xr_int(-1);
    if (nargs != 3 || !XR_IS_INT(args[0]) || !XR_IS_ARRAY(args[1]) || !XR_IS_INT(args[2]))
        return XR_CFUNC_DONE;
    XrArray *data = XR_TO_ARRAY(args[1]);
    int64_t offset = XR_TO_INT(args[2]);
    if (data->elem_type != XR_ELEM_U8 || data->length <= 0 || offset < 0 || offset >= data->length)
        return XR_CFUNC_DONE;
    if (!data->data)
        return XR_CFUNC_DONE;
    *result =
        xr_int(xr_http2_transport_send(X, XR_TO_INT(args[0]), (const uint8_t *) data->data + offset,
                                       (size_t) (data->length - offset)));
    return XR_CFUNC_DONE;
}

/* http2.__recv(handle, maxBytes, timeoutMs) -> Array<u8>?
 *
 * One read. A short read is normal and is answered as-is; reassembling frames
 * from short reads is http2.xr's job. An empty array means EOF; null means a
 * transport failure. The source layer maps those outcomes to public errors. */
static XrCFuncResult h2_recv(XrVMRuntime *X, XrValue *args, int nargs, XrValue *result) {
    *result = xr_null();
    if (nargs != 3 || !XR_IS_INT(args[0]) || !XR_IS_INT(args[1]) || !XR_IS_INT(args[2]))
        return XR_CFUNC_DONE;
    int64_t requested = XR_TO_INT(args[1]);
    int64_t timeout_value = XR_TO_INT(args[2]);
    if (requested <= 0 || requested > INT32_MAX || timeout_value <= 0 || timeout_value > INT32_MAX)
        return XR_CFUNC_DONE;

    XrCoroutine *coro = xr_current_coro(X);
    XrArray *buffer = xr_byte_array_new(coro, (int32_t) requested);
    if (!buffer)
        return XR_CFUNC_DONE;

    if (!buffer->data)
        return XR_CFUNC_DONE;
    int count = xr_http2_transport_recv(X, XR_TO_INT(args[0]), buffer->data, (size_t) requested,
                                        (int) timeout_value);
    if (count < 0)
        return XR_CFUNC_DONE;
    buffer->length = (int32_t) count;
    *result = xr_value_from_array(buffer);
    return XR_CFUNC_DONE;
}

/* http2.__close(handle) -> (); stale handles are already closed. */
static XrValue h2_close(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc != 1 || !XR_IS_INT(args[0]))
        return XR_NULL_VAL;
    xr_http2_transport_close(X, XR_TO_INT(args[0]));
    return XR_NULL_VAL;
}

#define XR_STDLIB_VM_BIND_MODULE_HTTP2 1
#include "../../src/stdlib/xstdlib_vm_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_MODULE_HTTP2
