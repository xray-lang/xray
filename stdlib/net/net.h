/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * net.h - Network standard library module entry point
 *
 * KEY CONCEPT:
 *   Thin script-binding entry. The IO state (netpoll, async pool, DNS
 *   cache) lives on XrRuntime; this header only exposes types the
 *   bindings use to materialise script-visible UDP addresses, address
 *   parsing/formatting helpers, and the module loader.
 */

#ifndef XR_STDLIB_NET_H
#define XR_STDLIB_NET_H

#include "../../src/base/xdefs.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "xneterror.h"

// Forward declarations
struct XrayIsolate;
struct XrModule;

/* ========== Network Address ========== */

typedef enum {
    XR_NET_IPV4,
    XR_NET_IPV6
} XrNetFamily;

typedef struct XrNetAddr {
    XrNetFamily family;
    char host[256];  // Hostname or IP string
    uint16_t port;
    union {
        uint8_t ipv4[4];
        uint8_t ipv6[16];
    } addr;
} XrNetAddr;

/* ========== Address parsing utilities ========== */

// Parse "host:port" into separate components.
XR_FUNC int xr_net_parse_addr(const char *addr_str, char *host, size_t host_len, int *port);

// Format an address back into "host:port".
XR_FUNC int xr_net_format_addr(const XrNetAddr *addr, char *buf, size_t buf_len);

/* ========== Module Loader ========== */

XR_FUNC struct XrModule *xr_load_module_net(struct XrayIsolate *isolate);

#endif  // XR_STDLIB_NET_H
