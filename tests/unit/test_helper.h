/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_helper.h - Test environment initialization helper
 *
 * KEY CONCEPT:
 *   Provides functions to initialize xray runtime for unit tests
 *   that require a full execution context (coroutine, GC, etc).
 */

#ifndef XR_TEST_HELPER_H
#define XR_TEST_HELPER_H

#include "xray.h"
#include "runtime/xisolate_internal.h"
#include "runtime/xisolate_api.h"
#include "coro/xcoroutine.h"
#include "vm/xvm_internal.h"

/*
 * Initialize test environment with a main coroutine
 * This creates a minimal closure and coroutine for testing.
 * 
 * Returns: main_coro pointer, or NULL on failure
 */
static inline XrCoroutine* xr_test_init_coro(XrayIsolate *X) {
    if (!X) return NULL;
    
    // If main_coro already exists, return it
    XrCoroutine *existing = xr_isolate_get_main_coro(X);
    if (existing) return existing;
    
    // Create a minimal proto for test coroutine
    XrProto *proto = (XrProto *)xr_malloc(sizeof(XrProto));
    if (!proto) return NULL;
    memset(proto, 0, sizeof(XrProto));
    
    // Initialize code dynarray with a single RETURN instruction
    xr_dynarray_init(&proto->code, sizeof(XrInstruction));
    XrInstruction ret_inst = 0;  // OP_RETURN encoded
    xr_dynarray_add_raw(&proto->code, &ret_inst);
    
    proto->maxstacksize = 8;
    proto->numparams = 0;
    proto->source_file = "<test>";
    
    // Create closure from proto
    XrClosure *closure = xr_closure_new(X, proto, NULL);
    if (!closure) {
        xr_dynarray_free(&proto->code);
        xr_free(proto);
        return NULL;
    }
    
    // Create main coroutine
    XrCoroutine *coro = xr_coro_create(X, closure, NULL, 0, "test_main", "<test>", 0);
    if (!coro) return NULL;
    
    xr_coro_flags_set(coro, XR_CORO_FLG_MAIN);
    xr_coro_upgrade_heap(coro, 0);
    xr_coro_sync_vm_ctx(coro, X);
    
    // Set as main_coro in isolate
    X->main_coro = coro;
    
    return coro;
}

/*
 * Get or create main coroutine for testing
 */
static inline XrCoroutine* xr_test_get_coro(XrayIsolate *X) {
    XrCoroutine *coro = xr_isolate_get_main_coro(X);
    if (!coro) {
        coro = xr_test_init_coro(X);
    }
    return coro;
}

#endif // XR_TEST_HELPER_H
