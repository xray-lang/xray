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
#include "../runtime/mem/xheap.h"
#include "../runtime/class/xclass.h"
#include "../runtime/class/xclass_system.h"
#include "../runtime/xisolate_api.h"
#include "../runtime/xisolate_internal.h"
#include "../runtime/mem/xsystem_heap.h"

#ifdef XR_ENABLE_TLS
#include "xtls_provider.h"
#endif

/* ========== Allocation helpers ========== */

static void *alloc_handle(struct XrVMRuntime *X, size_t size) {
    XR_DCHECK(X != NULL, "net_handle: alloc requires isolate");
    /*
     * Network handles and immutable TLS contexts are shared across coroutines.
     * Allocate them on the system shared heap like Channel.
     */
    XrSystemHeap *heap = xr_isolate_get_sys_heap(X);
    return heap ? xr_sysheap_alloc_shared(heap, size, XR_TINSTANCE) : NULL;
}

/* ========== Constructors ========== */

XrNetConn *xr_net_conn_new(struct XrVMRuntime *X, int fd, XrNetConnKind kind) {
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
    c->read_deadline_ms = 0;
    c->write_deadline_ms = 0;
    c->last_errno = 0;
    c->last_error = XR_NETERR_NONE;
    c->udp_from_host[0] = '\0';
    c->udp_from_port = 0;
    return c;
}

XrNetListener *xr_net_listener_new(struct XrVMRuntime *X, int fd, int port) {
    XrNetListener *l = (XrNetListener *) alloc_handle(X, sizeof(XrNetListener));
    if (!l)
        return NULL;
    XrayCoreClasses *core = xr_isolate_get_core_classes(X);
    l->klass = core ? core->netListenerClass : NULL;
    l->fd = fd;
    l->port = port;
    l->closed = false;
    l->isolate = X;
    l->accept_deadline_ms = 0;
    l->last_errno = 0;
    l->last_error = XR_NETERR_NONE;
    return l;
}

enum {
    XR_NET_TLS_CONTEXT_CLIENT = 1,
    XR_NET_TLS_CONTEXT_SERVER = 2,
};

static XrNetTlsContextHandle *new_tls_context_handle(struct XrVMRuntime *X, void *provider_context,
                                                     uint8_t role) {
    XrayCoreClasses *core = xr_isolate_get_core_classes(X);
    XrClass *klass = core ? core->tlsContextStorageClass : NULL;
    if (!provider_context || !klass)
        return NULL;
    XrNetTlsContextHandle *context =
        (XrNetTlsContextHandle *) alloc_handle(X, sizeof(XrNetTlsContextHandle));
    if (!context)
        return NULL;
    context->klass = klass;
    context->provider_context = provider_context;
    context->role = role;
    return context;
}

XrNetTlsContextHandle *xr_net_tls_client_context_handle_new(struct XrVMRuntime *X,
                                                            void *provider_context) {
    return new_tls_context_handle(X, provider_context, XR_NET_TLS_CONTEXT_CLIENT);
}

XrNetTlsContextHandle *xr_net_tls_server_context_handle_new(struct XrVMRuntime *X,
                                                            void *provider_context) {
    return new_tls_context_handle(X, provider_context, XR_NET_TLS_CONTEXT_SERVER);
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

XrNetConn *xr_net_conn_from_value(XrValue value) {
    if (!XR_IS_PTR(value) || XR_HEAP_TYPE(value) != XR_TINSTANCE)
        return NULL;
    XrNetConn *conn = (XrNetConn *) XR_VALUE_GCPTR(value);
    return conn->klass && conn->klass->builtin_kind == XR_BK_NETCONN ? conn : NULL;
}

static XrNetTlsContextHandle *tls_context_from_value(XrVMRuntime *X, XrValue value,
                                                     uint8_t expected_role) {
    XrayCoreClasses *core = xr_isolate_get_core_classes(X);
    XrClass *expected_class = core ? core->tlsContextStorageClass : NULL;
    if (!expected_class || !XR_IS_PTR(value) || XR_HEAP_TYPE(value) != XR_TINSTANCE)
        return NULL;
    XrNetTlsContextHandle *context = (XrNetTlsContextHandle *) XR_VALUE_GCPTR(value);
    return context->klass == expected_class && context->provider_context &&
                   context->role == expected_role
               ? context
               : NULL;
}

XrNetTlsContextHandle *xr_net_tls_client_context_from_value(XrVMRuntime *X, XrValue value) {
    return tls_context_from_value(X, value, XR_NET_TLS_CONTEXT_CLIENT);
}

XrNetTlsContextHandle *xr_net_tls_server_context_from_value(XrVMRuntime *X, XrValue value) {
    return tls_context_from_value(X, value, XR_NET_TLS_CONTEXT_SERVER);
}

void *xr_net_tls_context_provider(const XrNetTlsContextHandle *context) {
    return context ? context->provider_context : NULL;
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
 * object destroy hook and explicit close() call go through the same code
 * path. tls_state is freed first so the TLS layer can drain its own
 * buffers before the underlying fd vanishes.
 */

static void close_fd_with_netpoll(struct XrVMRuntime *X, int fd) {
    if (fd < 0)
        return;
    struct XrRuntime *rt = (X && X->vm.scheduler) ? (struct XrRuntime *) X->vm.scheduler : NULL;
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
    c->last_error = XR_NETERR_CLOSED;
    c->last_errno = 0;
}

void xr_net_listener_close(XrNetListener *l) {
    if (!l || l->closed)
        return;
    close_fd_with_netpoll(l->isolate, l->fd);
    l->fd = -1;
    l->closed = true;
    l->last_error = XR_NETERR_CLOSED;
    l->last_errno = 0;
}

/* ========== Native body destroy hooks ==========
 *
 * Called by xr_obj_destroy_instance via XrNativeBodyDesc.destroy.
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

static void tls_context_body_destroy(void *body) {
    XrNetTlsContextHandle *context =
        (XrNetTlsContextHandle *) ((char *) body -
                                   offsetof(XrNetTlsContextHandle, provider_context));
#ifdef XR_ENABLE_TLS
    xr_tls_context_free((XrTlsContext *) context->provider_context);
#endif
    context->provider_context = NULL;
}

/* ========== Native body descriptors ========== */

static XrNativeBodyDesc g_netconn_body_desc = {
    .body_size = sizeof(XrNetConn) - offsetof(XrNetConn, fd),
    .body_align = _Alignof(void *),
    .copy_policy = XR_NATIVE_BODY_COPY_FORBID,
    .init = NULL,
    .destroy = netconn_body_destroy,
    .deep_copy = NULL,
};

static XrNativeBodyDesc g_netlistener_body_desc = {
    .body_size = sizeof(XrNetListener) - offsetof(XrNetListener, fd),
    .body_align = _Alignof(void *),
    .copy_policy = XR_NATIVE_BODY_COPY_FORBID,
    .init = NULL,
    .destroy = netlistener_body_destroy,
    .deep_copy = NULL,
};

static XrNativeBodyDesc g_tls_context_body_desc = {
    .body_size = sizeof(XrNetTlsContextHandle) - offsetof(XrNetTlsContextHandle, provider_context),
    .body_align = _Alignof(void *),
    .copy_policy = XR_NATIVE_BODY_COPY_FORBID,
    .init = NULL,
    .destroy = tls_context_body_destroy,
    .deep_copy = NULL,
};

XrNativeBodyDesc *xr_netconn_body_desc(void) {
    return &g_netconn_body_desc;
}

XrNativeBodyDesc *xr_netlistener_body_desc(void) {
    return &g_netlistener_body_desc;
}

XrNativeBodyDesc *xr_tls_context_storage_body_desc(void) {
    return &g_tls_context_body_desc;
}
