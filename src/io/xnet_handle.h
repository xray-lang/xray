/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnet_handle.h - Typed network handle objects for scripts
 *
 * KEY CONCEPT:
 *   Replaces the old XrObjectInstance-based "{fd, type, tls}" handles that scripts
 *   could read by name. XrNetConn / XrNetListener are private opaque storage
 *   carrying the underlying fd plus type-specific state. Scripts operate
 *   on them only through the net module's byte primitives (readInto,
 *   writeBytes, accept, close, ...) and the registered handle methods.
 *
 * WHY THIS DESIGN:
 *   - Type safety: a TLS conn is never confused with a UDP socket; net
 *     APIs validate the heap type tag instead of trusting a script-set
 *     "type" field.
 *   - Lifecycle safety: close is idempotent and goes through a single
 *     destroy hook; scripts cannot reach in and clobber state.
 *   - Layering: handle types live in src/io/ so the runtime owns them
 *     without dragging stdlib into the GC type table.
 */

#ifndef XRAY_IO_NET_HANDLE_H
#define XRAY_IO_NET_HANDLE_H

#include <stdbool.h>
#include <stdint.h>
#include "../runtime/mem/xobj_header.h"
#include "../runtime/value/xvalue.h"

#ifdef __cplusplus
extern "C" {
#endif

struct XrVMRuntime;
struct XrCoroHeap;

/* ========== Connection kind ========== */

typedef enum {
    XR_NETCONN_TCP = 0, /* plain TCP stream            */
    XR_NETCONN_UDP = 1, /* UDP datagram socket         */
    XR_NETCONN_TLS = 2, /* TLS over TCP (tls_state set) */
} XrNetConnKind;

/*
 * Portable network error codes. The numbering is a stable script-facing
 * contract: net.__connLastCode / __listenerLastCode return these values verbatim and the NetError
 * classification table in stdlib/net/net.xr maps them to enum variants,
 * so renumbering is a breaking semantic change, not a refactor.
 */
typedef enum {
    XR_NETERR_NONE = 0,
    XR_NETERR_TIMEOUT = 1,
    XR_NETERR_CLOSED = 2,
    XR_NETERR_RESET = 3,
    XR_NETERR_REFUSED = 4,
    XR_NETERR_DNS = 5,
    XR_NETERR_TLS = 6,
    XR_NETERR_IO = 7,
    XR_NETERR_INVALID = 8,
    XR_NETERR_CANCELLED = 9,
} XrNetErrorKind;

/* ========== Connection handle ========== */

typedef struct XrNetConn {
    XrObjHeader gc_header;
    struct XrClass *klass;       /* unified class (builtin_kind == XR_BK_NET_CONN_STORAGE)   */
    int fd;                      /* -1 once closed                                 */
    uint8_t kind;                /* XrNetConnKind                                  */
    bool closed;                 /* idempotency guard for close                     */
    void *tls_state;             /* XrTlsConn* when kind == TLS, NULL otherwise     */
    struct XrVMRuntime *isolate; /* owning isolate (for netpoll cleanup) */
    int64_t read_deadline_ms;    /* absolute time.monotonic() ms, 0 = no deadline   */
    int64_t write_deadline_ms;   /* absolute time.monotonic() ms, 0 = no deadline   */
    int last_errno;              /* errno captured for the last failed operation    */
    uint8_t last_error;          /* XrNetErrorKind                                  */
    /* Sender of the last successful datagram receive. Scalar fields instead
     * of a composite return keep the primitive expressible on both the VM
     * and the AOT direct-call ABI; one reader per UDP socket is assumed. */
    char udp_from_host[46]; /* INET6_ADDRSTRLEN text, "" when none         */
    int udp_from_port;
} XrNetConn;

/* ========== Listener handle ========== */

typedef struct XrNetListener {
    XrObjHeader gc_header;
    struct XrClass *klass; /* unified class (builtin_kind == XR_BK_NET_LISTENER_STORAGE) */
    int fd;                /* -1 once closed                                 */
    int port;              /* listening port                                  */
    bool closed;
    struct XrVMRuntime *isolate;
    int64_t accept_deadline_ms; /* absolute time.monotonic() ms, 0 = no deadline */
    int last_errno;
    uint8_t last_error;
} XrNetListener;

/* TLS policies are immutable after construction. A managed handle keeps the
 * provider context alive while source code shares it across handshakes; the
 * native body destroy hook releases the provider exactly once. */
typedef struct XrNetTlsContextHandle {
    XrObjHeader gc_header;
    struct XrClass *klass;
    void *provider_context;
    uint8_t role;
} XrNetTlsContextHandle;

/* ========== Constructors ========== */

/*
 * Allocate an XrNetConn on the isolate's shared system heap. fd takes
 * ownership: callers must NOT close it directly after this returns.
 * Use xr_net_conn_close (or let the object destroy hook fire).
 */
XR_FUNC XrNetConn *xr_net_conn_new(struct XrVMRuntime *X, int fd, XrNetConnKind kind);

/*
 * Listener variant. port is informational (queryable by scripts).
 */
XR_FUNC XrNetListener *xr_net_listener_new(struct XrVMRuntime *X, int fd, int port);

XR_FUNC XrNetTlsContextHandle *xr_net_tls_client_context_handle_new(struct XrVMRuntime *X,
                                                                    void *provider_context);
XR_FUNC XrNetTlsContextHandle *xr_net_tls_server_context_handle_new(struct XrVMRuntime *X,
                                                                    void *provider_context);

/* ========== Accessors ========== */

XR_FUNC int xr_net_conn_fd(const XrNetConn *c);
XR_FUNC XrNetConnKind xr_net_conn_kind(const XrNetConn *c);
XR_FUNC bool xr_net_conn_is_tls(const XrNetConn *c);
XR_FUNC void *xr_net_conn_tls_state(const XrNetConn *c);
XR_FUNC bool xr_net_conn_is_closed(const XrNetConn *c);

/* Validate and unwrap the opaque script value without exposing its layout to
 * module-specific providers. */
XR_FUNC XrNetConn *xr_net_conn_from_value(XrValue value);
XR_FUNC XrNetTlsContextHandle *xr_net_tls_client_context_from_value(struct XrVMRuntime *X,
                                                                    XrValue value);
XR_FUNC XrNetTlsContextHandle *xr_net_tls_server_context_from_value(struct XrVMRuntime *X,
                                                                    XrValue value);
XR_FUNC void *xr_net_tls_context_provider(const XrNetTlsContextHandle *context);

XR_FUNC int xr_net_listener_fd(const XrNetListener *l);
XR_FUNC int xr_net_listener_port(const XrNetListener *l);
XR_FUNC bool xr_net_listener_is_closed(const XrNetListener *l);

/* ========== Mutators ========== */

/*
 * Promote a TCP conn to TLS by attaching the wrapped XrTlsConn* and
 * flipping the kind. Used by net.upgradeTLS.
 */
XR_FUNC void xr_net_conn_set_tls(XrNetConn *c, void *tls_state);

/*
 * Idempotent close: deregisters the fd from netpoll, closes it, and
 * marks the handle so subsequent operations short-circuit.
 */
XR_FUNC void xr_net_conn_close(XrNetConn *c);
XR_FUNC void xr_net_listener_close(XrNetListener *l);

/* ========== Native body descriptors ==========
 *
 * Wired into the XrClass via XrNativeBodyDesc. The destroy hook
 * mirrors xr_net_conn_close / xr_net_listener_close, so GC sweep
 * never leaks an fd if the script forgets to close.
 */
struct XrNativeBodyDesc;
XR_FUNC struct XrNativeBodyDesc *xr_netconn_body_desc(void);
XR_FUNC struct XrNativeBodyDesc *xr_netlistener_body_desc(void);
XR_FUNC struct XrNativeBodyDesc *xr_tls_context_storage_body_desc(void);

#ifdef __cplusplus
}
#endif

#endif /* XRAY_IO_NET_HANDLE_H */
