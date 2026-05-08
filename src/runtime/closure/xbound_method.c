/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xbound_method.c - Bound method allocation and value conversions
 */

#include "xbound_method.h"
#include "../../base/xchecks.h"
#include "../class/xenum.h"
#include "../class/xclass.h"
#include "../gc/xgc.h"
#include "../object/xiterator.h"
#include "../object/xnative_type.h"
#include "../symbol/xsymbol_table.h"
#include "../value/xtype_names.h"
#include "../xisolate_internal.h"

XrBoundMethod *xr_bound_method_new(XrayIsolate *isolate, XrValue receiver, MethodHandler handler) {
    XR_DCHECK(isolate != NULL, "bound_method_new: NULL isolate");
    XrBoundMethod *bm =
        (XrBoundMethod *) xr_gc_alloc(&isolate->gc, sizeof(XrBoundMethod), XR_TBOUND_METHOD);
    if (bm == NULL) {
        return NULL;
    }
    xr_gc_header_init_type(&bm->gc, XR_TBOUND_METHOD);
    bm->receiver = receiver;
    bm->handler = handler;
    return bm;
}

XrValue xr_value_from_bound_method(XrBoundMethod *bm) {
    return XR_FROM_PTR(bm);
}

bool xr_value_is_bound_method(XrValue v) {
    return XR_IS_BOUND_METHOD(v);
}

XrBoundMethod *xr_value_to_bound_method(XrValue v) {
    if (!XR_IS_BOUND_METHOD(v)) {
        return NULL;
    }
    return (XrBoundMethod *) XR_TO_PTR(v);
}

/* ========== Symbol -> MethodHandler bridges ========== */

/* Closure-taking methods (foreach / map / filter / reduce / find / ...)
 * and the lazy iterator entry need a bytecode-interpreting adapter
 * that hasn't been wired up to the bound-method path. Returning a real
 * value here would silently skip the user's callback; instead emit
 * XR_NOTFOUND so the VM's bound-method dispatcher in
 * xvm_dispatch_call.inc.c raises XR_ERR_TYPE_NO_METHOD with the
 * caller's PC. The error surfaces immediately at the call site
 * rather than masking the missing method as a null result. */
static XrValue bound_method_stub(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void) isolate;
    (void) receiver;
    (void) args;
    (void) argc;
    return XR_NOTFOUND;
}

/* Resolve a primitive method handler from native_type_classes.
 * Returns NULL if the type is not registered or the method is
 * not found / not a primitive. */
static MethodHandler resolve_native_handler(XrayIsolate *isolate, XrObjType gc_type, int symbol) {
    XR_DCHECK(isolate != NULL, "resolve_native_handler: NULL isolate");
    if ((int) gc_type >= XR_NATIVE_TYPE_MAX)
        return NULL;
    XrClass *cls = isolate->native_type_classes[gc_type];
    if (!cls)
        return NULL;
    XrMethod *m = xr_class_lookup_method(cls, symbol);
    if (m && m->type == XMETHOD_PRIMITIVE && m->as.primitive)
        return (MethodHandler) m->as.primitive;
    return NULL;
}

MethodHandler xr_map_get_handler(XrayIsolate *isolate, int symbol) {
    MethodHandler h = resolve_native_handler(isolate, XR_TMAP, symbol);
    if (h)
        return h;
    if (symbol == SYMBOL_FOREACH)
        return bound_method_stub;
    return NULL;
}

MethodHandler xr_array_get_handler(XrayIsolate *isolate, int symbol) {
    MethodHandler h = resolve_native_handler(isolate, XR_TARRAY, symbol);
    if (h)
        return h;
    if (symbol == SYMBOL_ITERATOR)
        return bound_method_stub;
    return NULL;
}

MethodHandler xr_set_get_handler(XrayIsolate *isolate, int symbol) {
    MethodHandler h = resolve_native_handler(isolate, XR_TSET, symbol);
    if (h)
        return h;
    if (symbol == SYMBOL_FOREACH || symbol == SYMBOL_MAP_METHOD || symbol == SYMBOL_FILTER)
        return bound_method_stub;
    return NULL;
}

MethodHandler xr_string_get_handler(XrayIsolate *isolate, int symbol) {
    MethodHandler h = resolve_native_handler(isolate, XR_TSTRING, symbol);
    if (h)
        return h;
    if (symbol == SYMBOL_ITERATOR)
        return bound_method_stub;
    return NULL;
}

/* Iterator methods touch receiver state (cursor advance), so they
 * stay outside the unified per-type table — wrap them directly. */
static XrValue iterator_hasnext_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args,
                                        int argc) {
    (void) isolate;
    (void) args;
    (void) argc;
    XrIterator *iter = xr_value_to_iterator(receiver);
    return xr_bool(xr_iterator_has_next(iter));
}

static XrValue iterator_next_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args,
                                     int argc) {
    (void) isolate;
    (void) args;
    (void) argc;
    XrIterator *iter = xr_value_to_iterator(receiver);
    return xr_iterator_next(iter);
}

MethodHandler xr_iterator_get_handler(int symbol) {
    if (symbol == SYMBOL_HASNEXT)
        return iterator_hasnext_handler;
    if (symbol == SYMBOL_NEXT)
        return iterator_next_handler;
    return NULL;
}

XrValue xr_enum_get_member_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args,
                                   int argc) {
    (void) isolate;
    if (argc < 1 || !XR_IS_INT(args[0]))
        return xr_null();
    if (!XR_IS_PTR(receiver))
        return xr_null();

    XrGCHeader *gc = (XrGCHeader *) XR_TO_PTR(receiver);
    if (XR_GC_GET_TYPE(gc) != XR_TENUM_TYPE)
        return xr_null();

    XrEnumType *enum_type = (XrEnumType *) gc;
    int index = XR_TO_INT(args[0]);
    if (index < 0 || index >= (int) enum_type->member_count) {
        return xr_null();
    }
    return XR_FROM_PTR(enum_type->members[index].instance);
}
