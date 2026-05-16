/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xclosure.h - Closure object (runtime first-class function value)
 *
 * KEY CONCEPT:
 *   XrClosure pairs a compiled prototype (XrProto) with captured upvalues.
 *   It is a heap-allocated, GC-managed first-class value used by all callers
 *   (VM, JIT, class methods, reflection). Closure lives at the runtime class
 *   layer so both lower runtime (value/object) and upper backends (vm/jit)
 *   can reference it without creating upward dependencies.
 */

#ifndef XCLOSURE_H
#define XCLOSURE_H

#include <stdint.h>
#include "../../base/xdefs.h"
#include "../value/xvalue.h"
#include "../value/xchunk.h"
#include "../gc/xgc_header.h"

struct XrayIsolate;
struct XrCoroutine;

// Function + captured environment (flat upvalue model).
// upvals[] trails the struct; its length equals `upval_count` and is
// determined by proto->upvalues at allocation time.
typedef struct XrClosure {
    XrGCHeader gc;
    XrProto *proto;        // compiled function prototype
    uint16_t upval_count;  // number of entries in upvals[]
    XrValue upvals[];      // flat upvalue array (const values + cell refs)
} XrClosure;

// Create a closure and allocate its upvalue array.
// `coro` selects the owning coroutine's heap (pass NULL to allocate on the
// isolate's GC heap for bootstrap paths).
XR_FUNC XrClosure *xr_closure_new(struct XrayIsolate *isolate, XrProto *proto,
                                  struct XrCoroutine *coro);

// Extract a closure pointer from a callback argument value. Returns the
// closure on success, or NULL after raising a runtime error tagged with
// `api_name` (e.g. "Array.reduce", "ws.serve") when the value isn't a
// function. All native APIs that accept callbacks must funnel through
// this helper instead of `XR_TO_PTR(v)` -- the latter blindly returns
// any non-zero payload as a pointer and crashes on non-pointer values.
XR_FUNC XrClosure *xr_closure_from_arg(struct XrayIsolate *isolate, XrValue v,
                                       const char *api_name);

#endif  // XCLOSURE_H
