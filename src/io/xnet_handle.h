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
 *   Replaces the old XrJson-based "{fd, type, tls}" handles that scripts
 *   could read by name. XrNetConn / XrNetListener are opaque GC objects
 *   carrying the underlying fd plus type-specific state. Scripts can
 *   only operate on them via the net.read / net.write / net.close
 *   entry points (eventually via instance methods on the registered
 *   native types).
 *
 * WHY THIS DESIGN:
 *   - Type safety: a TLS conn is never confused with a UDP socket; net
 *     APIs validate the GC type tag instead of trusting a script-set
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
#include "../runtime/gc/xgc_header.h"

#ifdef __cplusplus
extern "C" {
#endif

struct XrayIsolate;
struct XrCoroGC;

/* ========== Connection kind ========== */

typedef enum {
    XR_NETCONN_TCP = 0,  /* plain TCP stream            */
    XR_NETCONN_UDP = 1,  /* UDP datagram socket         */
    XR_NETCONN_TLS = 2,  /* TLS over TCP (tls_state set) */
} XrNetConnKind;

/* ========== Connection handle ========== */

typedef struct XrNetConn {
    XrGCHeader gc_header;
    int fd;             /* -1 once closed                                 */
    uint8_t kind;       /* XrNetConnKind                                  */
    bool closed;        /* idempotency guard for close                     */
    void *tls_state;    /* XrTlsConn* when kind == TLS, NULL otherwise     */
    struct XrayIsolate *isolate;  /* owning isolate (for netpoll cleanup) */
} XrNetConn;

/* ========== Listener handle ========== */

typedef struct XrNetListener {
    XrGCHeader gc_header;
    int fd;             /* -1 once closed                                 */
    int port;           /* listening port                                  */
    bool closed;
    struct XrayIsolate *isolate;
} XrNetListener;

/* ========== Constructors ========== */

/*
 * Allocate an XrNetConn on the calling coroutine's GC heap. fd takes
 * ownership: callers must NOT close it directly after this returns.
 * Use xr_net_conn_close (or let the GC destroy hook fire).
 */
XR_FUNC XrNetConn *xr_net_conn_new(struct XrayIsolate *X, int fd, XrNetConnKind kind);

/*
 * Listener variant. port is informational (queryable by scripts).
 */
XR_FUNC XrNetListener *xr_net_listener_new(struct XrayIsolate *X, int fd, int port);

/* ========== Accessors ========== */

XR_FUNC int xr_net_conn_fd(const XrNetConn *c);
XR_FUNC XrNetConnKind xr_net_conn_kind(const XrNetConn *c);
XR_FUNC bool xr_net_conn_is_tls(const XrNetConn *c);
XR_FUNC void *xr_net_conn_tls_state(const XrNetConn *c);
XR_FUNC bool xr_net_conn_is_closed(const XrNetConn *c);

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

/* ========== GC destroy hooks ==========
 *
 * Wired into the GC type table in src/runtime/gc/xgc.c. Mirrors what
 * xr_net_conn_close / xr_net_listener_close do, so finalisation never
 * leaks an fd if the script forgets to close.
 */
XR_FUNC void xr_gc_destroy_net_conn(XrGCHeader *obj, struct XrCoroGC *owning_gc);
XR_FUNC void xr_gc_destroy_net_listener(XrGCHeader *obj, struct XrCoroGC *owning_gc);

#ifdef __cplusplus
}
#endif

#endif /* XRAY_IO_NET_HANDLE_H */
