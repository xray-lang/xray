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
 *   cache) lives on XrRuntime; this public header only exposes the module
 *   loader. Network data-plane helpers stay private to the net module.
 */

#ifndef XR_STDLIB_NET_H
#define XR_STDLIB_NET_H

#include "../../src/base/xdefs.h"

// Forward declarations
struct XrVMRuntime;
struct XrModule;

/* ========== Module Loader ========== */


#endif  // XR_STDLIB_NET_H
