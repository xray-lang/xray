/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xset_methods.c - Set / WeakSet builtin method bodies + dispatch table.
 *
 * Methods that are unsupported on a weak set return XR_NOTFOUND so
 * the dispatcher's shared "method not found" path produces the
 * standard diagnostic.
 */

#include "xset_methods.h"
#include "xset.h"
#include "xarray.h"
#include "xiterator.h"
#include "xexception.h"
#include "xstring.h"
#include "../value/xvalue.h"
#include "../value/xvalue_format.h"
#include "../value/xtype_names.h"
#include "../symbol/xsymbol_table.h"
#include "../xerror_codes.h"
#include "../../coro/xcoroutine.h"
#include "../../base/xchecks.h"
#include "../../vm/xvm.h"

static inline XrSet *set_self(XrValue self) {
    XR_DCHECK(XR_IS_SET(self), "set method: receiver is not a Set");
    return XR_TO_SET(self);
}

static inline bool set_is_weak(const XrSet *s) {
    return (s->flags & XR_SET_FLAG_WEAK) != 0;
}

static XrValue xr_set_method_has(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    if (argc < 1)
        return xr_bool(0);
    return xr_bool(xr_set_has(set_self(self), args[0]));
}

static XrValue xr_set_method_delete(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    if (argc < 1)
        return xr_bool(0);
    return xr_bool(xr_set_delete(set_self(self), args[0]));
}

static XrValue xr_set_method_is_empty(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    return xr_bool(xr_set_is_empty(set_self(self)));
}

static XrValue xr_set_method_add(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    XrSet *s = set_self(self);
    /* WeakSet contract: value must be a heap object. */
    if (set_is_weak(s) && argc >= 1 && !XR_VALUE_NEEDS_GC(args[0])) {
        XrValue exc = xr_exception_newf(iso, XR_ERR_INVALID_ARG_TYPE,
                                        "WeakSet value must be a heap object, got %s",
                                        xr_typeid_name(xr_value_typeid(args[0])));
        xr_vm_unwind_with_trace(iso, exc);
        return xr_null();
    }
    if (argc < 1)
        return xr_bool(0);
    return xr_bool(xr_set_add(s, args[0]));
}

static XrValue xr_set_method_clear(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    XrSet *s = set_self(self);
    if (set_is_weak(s))
        return XR_NOTFOUND; /* not exposed on WeakSet */
    xr_set_clear(s);
    return xr_null();
}

static XrValue xr_set_method_union(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    XrSet *s = set_self(self);
    if (set_is_weak(s))
        return XR_NOTFOUND;
    if (argc < 1 || !XR_IS_SET(args[0]))
        return self;
    XrSet *result = xr_set_union(xr_current_coro(iso), s, XR_TO_SET(args[0]));
    return xr_value_from_set(result);
}

static XrValue xr_set_method_intersection(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    XrSet *s = set_self(self);
    if (set_is_weak(s))
        return XR_NOTFOUND;
    if (argc < 1 || !XR_IS_SET(args[0])) {
        return xr_value_from_set(xr_set_new(xr_current_coro(iso)));
    }
    XrSet *result = xr_set_intersection(xr_current_coro(iso), s, XR_TO_SET(args[0]));
    return xr_value_from_set(result);
}

static XrValue xr_set_method_difference(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    XrSet *s = set_self(self);
    if (set_is_weak(s))
        return XR_NOTFOUND;
    if (argc < 1 || !XR_IS_SET(args[0]))
        return self;
    XrSet *result = xr_set_difference(xr_current_coro(iso), s, XR_TO_SET(args[0]));
    return xr_value_from_set(result);
}

static XrValue xr_set_method_symmetric_difference(XrayIsolate *iso, XrValue self, XrValue *args,
                                                  int argc) {
    XrSet *s = set_self(self);
    if (set_is_weak(s))
        return XR_NOTFOUND;
    if (argc < 1 || !XR_IS_SET(args[0]))
        return self;
    XrSet *result = xr_set_symmetric_difference(xr_current_coro(iso), s, XR_TO_SET(args[0]));
    return xr_value_from_set(result);
}

static XrValue xr_set_method_is_subset(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    XrSet *s = set_self(self);
    if (set_is_weak(s))
        return XR_NOTFOUND;
    if (argc < 1 || !XR_IS_SET(args[0]))
        return xr_bool(0);
    return xr_bool(xr_set_is_subset(s, XR_TO_SET(args[0])));
}

static XrValue xr_set_method_is_superset(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    XrSet *s = set_self(self);
    if (set_is_weak(s))
        return XR_NOTFOUND;
    if (argc < 1 || !XR_IS_SET(args[0]))
        return xr_bool(0);
    return xr_bool(xr_set_is_superset(s, XR_TO_SET(args[0])));
}

static XrValue xr_set_method_to_array(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrSet *s = set_self(self);
    if (set_is_weak(s))
        return XR_NOTFOUND;
    XrArray *arr = xr_set_values(xr_current_coro(iso), s);
    return xr_value_from_array(arr);
}

static XrValue xr_set_method_iterator(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrSet *s = set_self(self);
    if (set_is_weak(s))
        return XR_NOTFOUND;
    XrIterator *iter = xr_iterator_new_from_set(xr_current_coro(iso), s);
    return iter ? xr_value_from_iterator(iter) : xr_null();
}

static XrValue xr_set_method_to_string(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    return xr_string_value(xr_value_to_string(iso, self));
}

/* ========== XrClass Registration ========== */

#include "xnative_type.h"
#include "builtins/xset_builtins.h"

void xr_set_register_native_type(XrayIsolate *isolate) {
    static const XrNativeMethod set_methods[] = {
        {"has", xr_set_method_has, 1},
        {"delete", xr_set_method_delete, 1},
        {"isEmpty", xr_set_method_is_empty, 0},
        {"add", xr_set_method_add, 0},
        {"clear", xr_set_method_clear, 0},
        {"union", xr_set_method_union, 0},
        {"intersection", xr_set_method_intersection, 0},
        {"difference", xr_set_method_difference, 0},
        {"symmetricDifference", xr_set_method_symmetric_difference, 0},
        {"isSubset", xr_set_method_is_subset, 0},
        {"isSuperset", xr_set_method_is_superset, 0},
        {"toArray", xr_set_method_to_array, 0},
        {"iterator", xr_set_method_iterator, 0},
        {"toString", xr_set_method_to_string, 0},
        {NULL, NULL, 0},
    };
    static const XrNativeMethod set_statics[] = {
        {"constructor", xr_builtin_set_construct, 0},
        {"from", xr_builtin_set_from, 1},
        {"range", xr_builtin_set_range, 2},
        {NULL, NULL, 0},
    };
    static const XrNativeTypeInfo set_info = {
        .name = "Set",
        .gc_type = XR_TSET,
        .methods = set_methods,
        .getters = NULL,
        .static_methods = set_statics,
    };
    xr_register_native_type(isolate, &set_info);
}
