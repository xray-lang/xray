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
#include "../../coro/xcoroutine.h"
#include "../gc/xgc.h"
#include "../gc/xcoro_gc.h"
#include "../value/xvalue.h"
#include "../xisolate_api.h"

/*
 * Create a closure with a flat upvalue array sized from proto->upvalues.
 * Upvalues are zero-initialised to null; callers populate them via OP_CLOSURE.
 */
XrClosure *xr_closure_new(XrayIsolate *isolate, XrProto *proto, struct XrCoroutine *coro) {
    XR_DCHECK(isolate != NULL, "closure_new: NULL isolate");
    XR_DCHECK(proto != NULL, "closure_new: NULL proto");

    int nuv = DYNARRAY_COUNT(&proto->upvalues);
    size_t size = sizeof(XrClosure) + (size_t) nuv * sizeof(XrValue);

    XrClosure *closure;
    if (coro && coro->coro_gc) {
        closure = (XrClosure *) xr_coro_gc_newobj(coro->coro_gc, XR_TFUNCTION, size);
    } else {
        closure = (XrClosure *) xr_gc_alloc(xr_isolate_get_gc(isolate), size, XR_TFUNCTION);
    }
    if (closure == NULL) {
        return NULL;
    }

    xr_gc_header_init_type(&closure->gc, XR_TFUNCTION);
    if (coro && coro->coro_gc) {
        XR_OBJ_SET_FLAG(&closure->gc, XR_OBJ_CYCLE_CANDIDATE);
    }
    closure->proto = proto;
    closure->upval_count = (uint16_t) nuv;

    for (int i = 0; i < nuv; i++) {
        closure->upvals[i] = xr_null();
    }

    return closure;
}

XR_FUNC void xr_gc_destroy_closure(XrGCHeader *obj, XrCoroGC *owning_gc) {
    if (!obj)
        return;
    XrClosure *closure = (XrClosure *) obj;
    for (uint16_t i = 0; i < closure->upval_count; i++) {
        xr_rc_release_value(owning_gc, closure->upvals[i]);
        closure->upvals[i] = xr_null();
    }
}
