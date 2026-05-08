/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmap_methods.c - Map / WeakMap method bodies + dispatch table.
 */

#include "xmap_methods.h"
#include "xmap.h"
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

static inline XrMap *map_self(XrValue self) {
    XR_DCHECK(XR_IS_MAP(self), "map method: receiver is not a Map");
    return XR_TO_MAP(self);
}

static inline bool map_is_weak(const XrMap *m) {
    return (m->flags & XR_MAP_FLAG_WEAK) != 0;
}

static XrValue xr_map_method_is_empty(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    return xr_bool(xr_map_is_empty(map_self(self)));
}

static XrValue xr_map_method_has(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    if (argc < 1)
        return xr_bool(0);
    return xr_bool(xr_map_has(map_self(self), args[0]));
}

static XrValue xr_map_method_get(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    if (argc < 1)
        return xr_null();
    bool found = false;
    XrValue result = xr_map_get(map_self(self), args[0], &found);
    return found ? result : xr_null();
}

static XrValue xr_map_method_set(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    XrMap *m = map_self(self);
    /* WeakMap contract: key must be a heap object. */
    if (map_is_weak(m) && argc >= 1 && !XR_VALUE_NEEDS_GC(args[0])) {
        XrValue exc = xr_exception_newf(iso, XR_ERR_INVALID_ARG_TYPE,
                                        "WeakMap key must be a heap object, got %s",
                                        xr_typeid_name(xr_value_typeid(args[0])));
        xr_vm_unwind_with_trace(iso, exc);
        return xr_null();
    }
    if (argc >= 2) {
        xr_map_set(m, args[0], args[1]);
    }
    return xr_null();
}

static XrValue xr_map_method_delete(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    if (argc < 1)
        return xr_bool(0);
    return xr_bool(xr_map_delete(map_self(self), args[0]));
}

static XrValue xr_map_method_clear(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    XrMap *m = map_self(self);
    if (map_is_weak(m))
        return XR_NOTFOUND;
    xr_map_clear(m);
    return xr_null();
}

static XrValue xr_map_method_keys(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrMap *m = map_self(self);
    if (map_is_weak(m))
        return XR_NOTFOUND;
    return xr_value_from_array(xr_map_keys(xr_current_coro(iso), m));
}

static XrValue xr_map_method_values(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrMap *m = map_self(self);
    if (map_is_weak(m))
        return XR_NOTFOUND;
    return xr_value_from_array(xr_map_values(xr_current_coro(iso), m));
}

static XrValue xr_map_method_entries(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrMap *m = map_self(self);
    if (map_is_weak(m))
        return XR_NOTFOUND;
    return xr_value_from_array(xr_map_entries(xr_current_coro(iso), m));
}

static XrValue xr_map_method_has_value(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    XrMap *m = map_self(self);
    if (map_is_weak(m))
        return XR_NOTFOUND;
    if (argc < 1)
        return xr_bool(0);
    return xr_bool(xr_map_has_value(m, args[0]));
}

static XrValue xr_map_method_iterator(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    XrMap *m = map_self(self);
    if (map_is_weak(m))
        return XR_NOTFOUND;
    /* Legacy semantics: regular map's iterator() returns null;
     * for-in iteration uses entriesIterator. */
    return xr_null();
}

static XrValue xr_map_method_entries_iterator(XrayIsolate *iso, XrValue self, XrValue *args,
                                              int argc) {
    (void) args;
    (void) argc;
    XrMap *m = map_self(self);
    if (map_is_weak(m))
        return XR_NOTFOUND;
    XrIterator *iter = xr_map_entries_iterator(iso, m);
    return iter ? xr_value_from_iterator(iter) : xr_null();
}

static XrValue xr_map_method_to_string(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    return xr_string_value(xr_value_to_string(iso, self));
}

const XrMethodSlot xr_map_method_table[SYMBOL_BUILTIN_COUNT] = {
    [SYMBOL_IS_EMPTY] = {xr_map_method_is_empty, 0, 0, XR_METHOD_FLAG_PURE | XR_METHOD_FLAG_NO_GC},
    [SYMBOL_HAS] = {xr_map_method_has, 1, 1, 0},
    [SYMBOL_GET] = {xr_map_method_get, 1, 1, 0},
    [SYMBOL_SET] = {xr_map_method_set, 0, 2, XR_METHOD_FLAG_MAY_THROW},
    [SYMBOL_DELETE] = {xr_map_method_delete, 1, 1, 0},
    [SYMBOL_CLEAR] = {xr_map_method_clear, 0, 0, 0},
    [SYMBOL_KEYS] = {xr_map_method_keys, 0, 0, 0},
    [SYMBOL_VALUES] = {xr_map_method_values, 0, 0, 0},
    [SYMBOL_ENTRIES] = {xr_map_method_entries, 0, 0, 0},
    [SYMBOL_HAS_VALUE_MAP] = {xr_map_method_has_value, 1, 1, 0},
    [SYMBOL_ITERATOR] = {xr_map_method_iterator, 0, 0, XR_METHOD_FLAG_PURE | XR_METHOD_FLAG_NO_GC},
    [SYMBOL_ENTRIES_ITERATOR] = {xr_map_method_entries_iterator, 0, 0, 0},
    [SYMBOL_TOSTRING] = {xr_map_method_to_string, 0, 0, XR_METHOD_FLAG_MAY_THROW},
};

/* ========== XrClass Registration ========== */

#include "xnative_type.h"

void xr_map_register_native_type(XrayIsolate *isolate) {
    static const XrNativeMethod map_methods[] = {
        {"isEmpty", xr_map_method_is_empty, 0},
        {"has", xr_map_method_has, 1},
        {"get", xr_map_method_get, 1},
        {"set", xr_map_method_set, 0},
        {"delete", xr_map_method_delete, 1},
        {"clear", xr_map_method_clear, 0},
        {"keys", xr_map_method_keys, 0},
        {"values", xr_map_method_values, 0},
        {"entries", xr_map_method_entries, 0},
        {"hasValue", xr_map_method_has_value, 1},
        {"iterator", xr_map_method_iterator, 0},
        {"entriesIterator", xr_map_method_entries_iterator, 0},
        {"toString", xr_map_method_to_string, 0},
        {NULL, NULL, 0},
    };
    static const XrNativeTypeInfo map_info = {
        .name = "Map",
        .gc_type = XR_TMAP,
        .methods = map_methods,
        .getters = NULL,
        .static_methods = NULL,
    };
    xr_register_native_type(isolate, &map_info);
}
