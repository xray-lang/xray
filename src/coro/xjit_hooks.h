/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xjit_hooks.h - JIT callback interface for coro module (decoupling shim)
 *
 * KEY CONCEPT:
 *   coro/ (L3) must not include jit/ (L5) headers directly.
 *   This vtable is registered by jit/ at init time; coro/ calls
 *   through the function pointers.  When XRAY_HAS_JIT is off or
 *   JIT is not initialized, xr_jit_hooks is NULL and all call
 *   sites degrade to interpreter-only paths.
 */

#ifndef XJIT_HOOKS_H
#define XJIT_HOOKS_H

#include "../base/xdefs.h"
#include "../runtime/value/xvalue.h"
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>

/* Forward declarations (avoid pulling in full headers) */
struct XrCoroutine;
struct XrProto;
struct XrType;
struct XrayIsolate;

/* ========== JIT result codes (mirror jit/xm_jit.h constants) ========== */

enum {
    XR_JIT_OK = 0,
    XR_JIT_DEOPT = 1,
    XR_JIT_SUSPEND = 2,
};

/* ========== JIT Hooks Vtable ========== */

typedef struct XrJitHooks {
    /* --- Execution --- */

    // Call compiled code for a function entry.
    // Mirrors xm_jit_call() signature.
    int (*call)(void *entry, struct XrCoroutine *coro, XrValue *args, int nargs,
                struct XrType *return_type_info, XrValue *result);

    // Resume from a JIT suspend point.
    // Mirrors xm_jit_resume() signature.
    int (*resume)(struct XrCoroutine *coro, XrValue *result);

    // Install background-compiled JIT result into proto.
    // Mirrors xm_jit_install_bg_result() signature.
    void (*install_bg_result)(struct XrProto *proto);

    // Try to compile proto for an isolate-owned JIT state.
    bool (*try_compile)(struct XrayIsolate *isolate, struct XrProto *proto);

    // Return whether proto contains exception control that should bypass direct JIT entry.
    bool (*proto_has_exception_control)(const struct XrProto *proto);

    /* --- Guard page safepoint --- */

    void *(*guard_page_alloc)(void);
    void (*guard_page_free)(void *page);
    void (*guard_page_arm)(void *page);
    void (*guard_page_disarm)(void *page);

    /* --- Worker-local scratch storage --- */

    void *(*worker_state_create)(void);
    void (*worker_state_destroy)(void *worker_state);
    void *(*worker_scratch)(void *worker_state);
    void *(*worker_safepoint_page)(void *worker_state);
    void (*worker_set_heartbeat)(void *worker_state, _Atomic uint64_t *heartbeat);
} XrJitHooks;

/* Global hooks pointer.  NULL when JIT is not available. */
XR_DATA XrJitHooks *xr_jit_hooks;

/* ========== Convenience macros ========== */

#define XR_JIT_AVAILABLE() (xr_jit_hooks != NULL)

#endif  // XJIT_HOOKS_H
