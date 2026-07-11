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
#include "../mem/xheap.h"
#include "../mem/xcoro_heap.h"
#include "../value/xvalue.h"
#include "../xisolate_api.h"

/*
 * Create a closure with a flat upvalue array sized from proto->upvalues.
 * Upvalues are zero-initialised to null; callers populate them via OP_CLOSURE.
 */
XrClosure *xr_closure_new(XrVMRuntime *isolate, XrProto *proto, struct XrCoroutine *coro) {
    XR_DCHECK(isolate != NULL, "closure_new: NULL isolate");
    XR_DCHECK(proto != NULL, "closure_new: NULL proto");

    int nuv = DYNARRAY_COUNT(&proto->upvalues);
    size_t size = sizeof(XrClosure) + (size_t) nuv * sizeof(XrValue);

    XrClosure *closure = (XrClosure *) xr_alloc(coro, size, XR_TFUNCTION);
    if (closure == NULL) {
        return NULL;
    }

    xr_obj_header_init_type(&closure->hdr, XR_TFUNCTION);
    if (coro) {
        XR_OBJ_SET_FLAG(&closure->hdr, XR_OBJ_CYCLE_CANDIDATE);
    }
    closure->proto = proto;
    closure->upval_count = (uint16_t) nuv;

    for (int i = 0; i < nuv; i++) {
        closure->upvals[i] = xr_null();
    }

    return closure;
}

XR_FUNC void xr_obj_destroy_closure(XrObjHeader *obj, XrCoroHeap *owner_heap) {
    if (!obj)
        return;
    XrClosure *closure = (XrClosure *) obj;
    for (uint16_t i = 0; i < closure->upval_count; i++) {
        xr_rc_release_value(owner_heap, closure->upvals[i]);
        closure->upvals[i] = xr_null();
    }
}
