/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_net_resolve.h - Owned DNS requests for hosted native execution
 *
 * KEY CONCEPT:
 *   Blocking lookup produces only plain address data. The resumed caller
 *   materializes execution-local strings and arrays in its own arena.
 */

#ifndef XRT_NET_RESOLVE_H
#define XRT_NET_RESOLVE_H

#include "xrt_net.h"

typedef struct xrt_net_resolved_addr {
    int family;
    union { struct in_addr v4; struct in6_addr v6; } addr;
} xrt_net_resolved_addr_t;

typedef struct xrt_net_resolve_candidates {
    int error;
    int count;
    xrt_net_resolved_addr_t addresses[XRT_NET_RESOLVE_MAX_ADDRS];
} xrt_net_resolve_candidates_t;

/* The pool and coroutine each hold one owner. No field borrows a language
 * object or a coroutine frame, so late completion after cancellation is safe. */
typedef struct xrt_net_resolve_request {
    atomic_uint owners;
    atomic_bool complete;
    char *hostname;
    xrt_net_resolve_candidates_t candidates;
} xrt_net_resolve_request_t;

static inline void xrt_net_resolve_lookup(const char *hostname,
                                          xrt_net_resolve_candidates_t *out) {
    memset(out, 0, sizeof(*out));
    if (!hostname || !hostname[0]) {
        out->error = EAI_NONAME;
        return;
    }
    xrt_net_init_once();
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *addresses = NULL;
    out->error = getaddrinfo(hostname, NULL, &hints, &addresses);
    if (out->error != 0 || !addresses)
        return;
    xrt_net_resolved_addr_t v6[XRT_NET_RESOLVE_MAX_ADDRS], v4[XRT_NET_RESOLVE_MAX_ADDRS];
    int n6 = 0, n4 = 0;
    for (const struct addrinfo *item = addresses; item; item = item->ai_next) {
        if (item->ai_family == AF_INET6 && n6 < XRT_NET_RESOLVE_MAX_ADDRS) {
            v6[n6].family = AF_INET6;
            v6[n6++].addr.v6 = ((const struct sockaddr_in6 *) item->ai_addr)->sin6_addr;
        } else if (item->ai_family == AF_INET && n4 < XRT_NET_RESOLVE_MAX_ADDRS) {
            v4[n4].family = AF_INET;
            v4[n4++].addr.v4 = ((const struct sockaddr_in *) item->ai_addr)->sin_addr;
        }
    }
    freeaddrinfo(addresses);
    int i6 = 0, i4 = 0;
    while (out->count < XRT_NET_RESOLVE_MAX_ADDRS && (i6 < n6 || i4 < n4)) {
        if (i6 < n6)
            out->addresses[out->count++] = v6[i6++];
        if (out->count < XRT_NET_RESOLVE_MAX_ADDRS && i4 < n4)
            out->addresses[out->count++] = v4[i4++];
    }
}

static inline XrValue xrt_net_resolve_materialize(const xrt_net_resolve_candidates_t *candidates) {
    xrt_net_array_view_t *out = xrt_net_array_alloc_any(XRT_NET_RESOLVE_MAX_ADDRS);
    XrValue *items = (XrValue *) out->data;
    for (int i = 0; candidates && i < candidates->count; i++) {
        const xrt_net_resolved_addr_t *candidate = &candidates->addresses[i];
        char text[INET6_ADDRSTRLEN];
        const void *address = candidate->family == AF_INET ? (const void *) &candidate->addr.v4
                                                          : (const void *) &candidate->addr.v6;
        if (!inet_ntop(candidate->family, address, text, sizeof(text)))
            continue;
        size_t length = strlen(text);
        XrValue element = xrt_str_alloc(length);
        if (length)
            memcpy(xr_str_buf(element), text, length);
        items[out->length++] = element;
        out->contains_refs = 1;
    }
    return xr_mkptr(out, XR_TAG_ARRAY);
}

static inline XrValue xrt_net_resolve_all(const char *hostname, int64_t length) {
    char *owned_hostname = xrt_net_cstr_dup_arg(hostname, length);
    xrt_net_resolve_candidates_t candidates;
    xrt_net_resolve_lookup(owned_hostname, &candidates);
    XRT_FREE(owned_hostname);
    return xrt_net_resolve_materialize(&candidates);
}

static inline xrt_net_resolve_request_t *xrt_net_resolve_request_new(const char *hostname,
                                                                     int64_t length) {
    xrt_net_resolve_request_t *request = (xrt_net_resolve_request_t *) XRT_MALLOC(sizeof(*request));
    if (!request)
        return NULL;
    request->hostname = xrt_net_cstr_dup_arg(hostname, length);
    if (!request->hostname) {
        XRT_FREE(request);
        return NULL;
    }
    atomic_init(&request->owners, 1u);
    atomic_init(&request->complete, false);
    memset(&request->candidates, 0, sizeof(request->candidates));
    return request;
}

static inline void xrt_net_resolve_request_retain(xrt_net_resolve_request_t *request) {
    atomic_fetch_add_explicit(&request->owners, 1u, memory_order_relaxed);
}

static inline void xrt_net_resolve_request_release(void *data) {
    xrt_net_resolve_request_t *request = (xrt_net_resolve_request_t *) data;
    if (request && atomic_fetch_sub_explicit(&request->owners, 1u, memory_order_acq_rel) == 1u) {
        XRT_FREE(request->hostname);
        XRT_FREE(request);
    }
}

static inline void xrt_net_resolve_request_invoke(void *data) {
    xrt_net_resolve_request_t *request = (xrt_net_resolve_request_t *) data;
    xrt_net_resolve_lookup(request->hostname, &request->candidates);
    atomic_store_explicit(&request->complete, true, memory_order_release);
}

static inline bool xrt_net_resolve_request_is_complete(const xrt_net_resolve_request_t *request) {
    return request && atomic_load_explicit(&request->complete, memory_order_acquire);
}

#endif // XRT_NET_RESOLVE_H
