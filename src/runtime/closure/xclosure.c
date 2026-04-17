/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xclosure.c - Closure object allocation and lifetime
 */

#include "xclosure.h"
#include "../../base/xchecks.h"
#include "../gc/xgc.h"
#include "../gc/xcoro_gc.h"
#include "../xisolate_internal.h"
#include "../../coro/xcoroutine.h"

/*
 * Create a closure with a flat upvalue array sized from proto->upvalues.
 * Upvalues are zero-initialised to null; callers populate them via OP_CLOSURE
 * or the JIT closure-creation path.
 */
XrClosure *xr_closure_new(XrayIsolate *isolate, XrProto *proto,
                          struct XrCoroutine *coro) {
    XR_DCHECK(isolate != NULL, "closure_new: NULL isolate");
    XR_DCHECK(proto != NULL, "closure_new: NULL proto");

    int nuv = DYNARRAY_COUNT(&proto->upvalues);
    size_t size = sizeof(XrClosure) + (size_t)nuv * sizeof(XrValue);

    XrClosure *closure;
    if (coro && coro->coro_gc) {
        closure = (XrClosure *)xr_coro_gc_newobj(coro->coro_gc, XR_TFUNCTION, size);
    } else {
        closure = (XrClosure *)xr_gc_alloc(&isolate->gc, size, XR_TFUNCTION);
    }
    if (closure == NULL) {
        return NULL;
    }

    xr_gc_header_init_type(&closure->gc, XR_TFUNCTION);
    closure->proto = proto;
    closure->upval_count = (uint16_t)nuv;

    for (int i = 0; i < nuv; i++) {
        closure->upvals[i] = xr_null();
    }

    return closure;
}
