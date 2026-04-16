/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcoro_registry.h - Named coroutine registry and lifecycle monitoring
 *
 * KEY CONCEPT:
 *   Hash table mapping coroutine names to coroutine pointers.
 *   Monitors receive exit notifications via Channel when watched
 *   coroutines terminate.
 *
 * WHY THIS DESIGN:
 *   - Coroutines with go(name: "xxx") auto-register
 *   - Lightweight alternative to full Actor model
 *   - Uses existing Channel for async notification (no mailbox needed)
 *
 * RELATED MODULES:
 *   - xcoroutine.h: XrCoroutine.watched_by field
 *   - xchannel.h: Monitor notification Channel
 *   - xworker.c: Exit path hooks
 */

#ifndef XCORO_REGISTRY_H
#define XCORO_REGISTRY_H

#include <stdbool.h>
#include <stdint.h>
#include "xchannel.h"

/* ========== Forward Declarations ========== */

typedef struct XrCoroutine XrCoroutine;
typedef struct XrayIsolate XrayIsolate;
typedef struct XrChannel XrChannel;

/* ========== Monitor Entry (intrusive linked list per coroutine) ========== */

typedef struct XrCoroMonitor {
    XrChannel *channel;              // notification channel (owned by caller)
    struct XrCoroMonitor *next;
} XrCoroMonitor;

/* ========== Registry Entry ========== */

typedef struct XrCoroRegEntry {
    const char *name;                // key (borrowed from XrCoroutine.name)
    XrCoroutine *coro;               // value
    uint32_t hash;                   // cached hash (0 = empty slot)
} XrCoroRegEntry;

/* ========== Registry (per-isolate) ========== */

#define XR_CORO_REG_INITIAL_CAP 32
#define XR_CORO_REG_LOAD_FACTOR 75   // percent

typedef struct XrCoroRegistry {
    XrCoroRegEntry *entries;
    uint32_t capacity;
    uint32_t count;
    XrSpinlock lock;
} XrCoroRegistry;

/* ========== Registry API ========== */

// Initialize/destroy registry
XR_FUNC void xr_coro_registry_init(XrCoroRegistry *reg);
XR_FUNC void xr_coro_registry_destroy(XrCoroRegistry *reg);

// Register a named coroutine (returns false if name already taken)
XR_FUNC bool xr_coro_registry_register(XrCoroRegistry *reg, const char *name, XrCoroutine *coro);

// Unregister by name (called on coroutine exit)
XR_FUNC void xr_coro_registry_unregister(XrCoroRegistry *reg, const char *name);

// Look up coroutine by name (returns NULL if not found)
XR_FUNC XrCoroutine *xr_coro_registry_whereis(XrCoroRegistry *reg, const char *name);

/* ========== Monitor API ========== */

// Monitor a named coroutine: returns a Channel that receives exit info.
// If coroutine not found or already dead, sends immediate notification.
// Caller owns the returned channel.
XR_FUNC XrChannel *xr_coro_monitor(XrayIsolate *X, XrCoroRegistry *reg, const char *name);

// Remove a monitor by channel (demonitor). Acquires registry lock.
XR_FUNC void xr_coro_demonitor(XrCoroRegistry *reg, XrCoroutine *coro, XrChannel *ch);

// Notify all monitors that a coroutine has exited. Acquires registry lock.
// Called from exit path in xworker.c.
// reason: "normal", "error", "cancelled"
XR_FUNC void xr_coro_notify_monitors(XrayIsolate *X, XrCoroRegistry *reg, XrCoroutine *coro, const char *reason);

// Auto-unregister named coroutine on exit.
// Called from exit path in xworker.c after notify_monitors.
XR_FUNC void xr_coro_on_exit(XrayIsolate *X, XrCoroutine *coro);

#endif // XCORO_REGISTRY_H
