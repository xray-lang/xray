/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnet_handle.c - Typed network handle implementation
 */

#include "xnet_handle.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../coro/xcoroutine.h"
#include "../coro/xnetpoll.h"
#include "../coro/xworker.h"
#include "../os/os_net.h"
#include "../runtime/gc/xgc.h"
#include "../runtime/class/xclass.h"
#include "../runtime/class/xclass_system.h"
#include "../runtime/xisolate_api.h"
#include "../runtime/xisolate_internal.h"

#ifdef XR_ENABLE_TLS
#include "../../stdlib/net/tls.h"
#endif

/* ========== Allocation helpers ========== */

static void *alloc_handle(struct XrayIsolate *X, size_t size) {
    XR_DCHECK(X != NULL, "net_handle: alloc requires isolate");
    /*
     * Allocate on the calling coroutine's heap when available, mirror
     * the xinstance / xexception pattern of falling back to the
     * isolate gc for bootstrap paths that have no current coroutine
     * (e.g. CLI / tests creating handles outside a scheduler).
     */
    struct XrCoroutine *coro = xr_current_coro(X);
    void *obj = NULL;
    if (coro) {
        obj = xr_alloc(coro, size, (uint8_t) XR_TINSTANCE);
    } else {
        obj = xr_gc_alloc(xr_isolate_get_gc(X), size, (uint8_t) XR_TINSTANCE);
    }
    return obj;
}

/* ========== Constructors ========== */

XrNetConn *xr_net_conn_new(struct XrayIsolate *X, int fd, XrNetConnKind kind) {
    XrNetConn *c = (XrNetConn *) alloc_handle(X, sizeof(XrNetConn));
    if (!c)
        return NULL;
    XrayCoreClasses *core = xr_isolate_get_core_classes(X);
    c->klass = core ? core->netConnClass : NULL;
    c->fd = fd;
    c->kind = (uint8_t) kind;
    c->closed = false;
    c->tls_state = NULL;
    c->isolate = X;
    return c;
}

XrNetListener *xr_net_listener_new(struct XrayIsolate *X, int fd, int port) {
    XrNetListener *l = (XrNetListener *) alloc_handle(X, sizeof(XrNetListener));
    if (!l)
        return NULL;
    XrayCoreClasses *core = xr_isolate_get_core_classes(X);
    l->klass = core ? core->netListenerClass : NULL;
    l->fd = fd;
    l->port = port;
    l->closed = false;
    l->isolate = X;
    return l;
}

/* ========== Accessors ========== */

int xr_net_conn_fd(const XrNetConn *c) {
    return c ? c->fd : -1;
}

XrNetConnKind xr_net_conn_kind(const XrNetConn *c) {
    return c ? (XrNetConnKind) c->kind : XR_NETCONN_TCP;
}

bool xr_net_conn_is_tls(const XrNetConn *c) {
    return c && c->kind == XR_NETCONN_TLS;
}

void *xr_net_conn_tls_state(const XrNetConn *c) {
    return c ? c->tls_state : NULL;
}

bool xr_net_conn_is_closed(const XrNetConn *c) {
    return !c || c->closed || c->fd < 0;
}

int xr_net_listener_fd(const XrNetListener *l) {
    return l ? l->fd : -1;
}

int xr_net_listener_port(const XrNetListener *l) {
    return l ? l->port : -1;
}

bool xr_net_listener_is_closed(const XrNetListener *l) {
    return !l || l->closed || l->fd < 0;
}

/* ========== Mutators ========== */

void xr_net_conn_set_tls(XrNetConn *c, void *tls_state) {
    if (!c)
        return;
    c->tls_state = tls_state;
    c->kind = (uint8_t) XR_NETCONN_TLS;
}

/* ========== Close paths ==========
 *
 * Centralises the "deregister from netpoll, close fd" sequence so the
 * GC destroy hook and explicit close() call go through the same code
 * path. tls_state is freed first so the TLS layer can drain its own
 * buffers before the underlying fd vanishes.
 */

static void close_fd_with_netpoll(struct XrayIsolate *X, int fd) {
    if (fd < 0)
        return;
    struct XrRuntime *rt = (X && X->vm.runtime) ? (struct XrRuntime *) X->vm.runtime : NULL;
    if (rt) {
        XrPollDesc *pd = xr_fdmap_get(&rt->netpoll, fd);
        if (pd)
            xr_netpoll_close(&rt->netpoll, pd);
    }
    xr_closesocket(fd);
}

void xr_net_conn_close(XrNetConn *c) {
    if (!c || c->closed)
        return;
#ifdef XR_ENABLE_TLS
    if (c->tls_state) {
        xr_tls_conn_free((XrTlsConn *) c->tls_state);
        c->tls_state = NULL;
    }
#endif
    close_fd_with_netpoll(c->isolate, c->fd);
    c->fd = -1;
    c->closed = true;
}

void xr_net_listener_close(XrNetListener *l) {
    if (!l || l->closed)
        return;
    close_fd_with_netpoll(l->isolate, l->fd);
    l->fd = -1;
    l->closed = true;
}

/* ========== Native body destroy hooks ==========
 *
 * Called by xr_gc_destroy_instance via XrNativeBodyDesc.destroy.
 * The body pointer points to the first field after klass (i.e. fd),
 * so we recover the enclosing struct by subtracting the body offset.
 */

static void netconn_body_destroy(void *body) {
    XrNetConn *c = (XrNetConn *) ((char *) body - offsetof(XrNetConn, fd));
    xr_net_conn_close(c);
}

static void netlistener_body_destroy(void *body) {
    XrNetListener *l = (XrNetListener *) ((char *) body - offsetof(XrNetListener, fd));
    xr_net_listener_close(l);
}

/* ========== Native body descriptors ========== */

static XrNativeBodyDesc g_netconn_body_desc = {
    .body_size = sizeof(XrNetConn) - offsetof(XrNetConn, fd),
    .body_align = _Alignof(void *),
    .copy_policy = XR_NATIVE_BODY_COPY_FORBID,
    .init = NULL,
    .destroy = netconn_body_destroy,
    .traverse = NULL,
    .deep_copy = NULL,
    .to_shared = NULL,
};

static XrNativeBodyDesc g_netlistener_body_desc = {
    .body_size = sizeof(XrNetListener) - offsetof(XrNetListener, fd),
    .body_align = _Alignof(void *),
    .copy_policy = XR_NATIVE_BODY_COPY_FORBID,
    .init = NULL,
    .destroy = netlistener_body_destroy,
    .traverse = NULL,
    .deep_copy = NULL,
    .to_shared = NULL,
};

XrNativeBodyDesc *xr_netconn_body_desc(void) {
    return &g_netconn_body_desc;
}

XrNativeBodyDesc *xr_netlistener_body_desc(void) {
    return &g_netlistener_body_desc;
}
