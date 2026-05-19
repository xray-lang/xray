/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xjson.c - Json object backed by dynamic-layout XrInstance
 *
 * Json values are XrInstance with classes in the dynamic-layout chain
 * rooted at core->jsonRootClass. The actual field storage, transitions,
 * overflow, and GC traversal all live in xinstance.c / xclass.c.
 */

#include "xjson.h"
#include "../../base/xchecks.h"
#include "../gc/xgc.h"
#include "../gc/xalloc_unified.h"
#include "../../base/xmalloc.h"
#include "../class/xinstance.h"
#include "../coro/xcoroutine.h"
#include "../xisolate_api.h"
#include "../xisolate_internal.h"
#include "../symbol/xsymbol_table.h"
#include <string.h>
#include <stdio.h>

static inline XrSymbolTable *get_symbol_table(XrayIsolate *isolate) {
    return (XrSymbolTable *) xr_isolate_get_symbol_table(isolate);
}

static inline XrClass *json_root_class(XrayIsolate *X) {
    XR_DCHECK(X && X->core && X->core->jsonRootClass, "json: root class not initialized");
    return X->core->jsonRootClass;
}

/* ========== Creation API ========== */

XrJson *xr_json_new(struct XrCoroutine *coro) {
    XR_DCHECK(coro != NULL, "json_new: NULL coro");
    XrayIsolate *X = xr_coro_get_isolate(coro);
    XrClass *cls = json_root_class(X);
    return xr_json_new_with_class(coro, cls);
}

XrJson *xr_json_new_with_class(struct XrCoroutine *coro, XrClass *cls) {
    XR_DCHECK(coro != NULL, "json_new_with_class: NULL coro");
    XR_DCHECK(cls != NULL, "json_new_with_class: NULL class");
    XR_DCHECK(cls->flags & XR_CLASS_DYNAMIC_LAYOUT, "json_new_with_class: not dynamic-layout");
    XrayIsolate *X = xr_coro_get_isolate(coro);

    // Json objects are XR_TINSTANCE with a dynamic-layout class carrying
    // XR_CLASS_JSON; xr_value_is_json checks the class flag.
    size_t size = xr_instance_size(cls);
    XrJson *json = (XrJson *) xr_alloc(coro, size, XR_TINSTANCE);
    if (!json)
        return NULL;
    xr_gc_header_init_type(&json->gc, XR_TINSTANCE);
    json->klass = cls;
    uint16_t cap = cls->in_object_capacity;
    for (uint16_t i = 0; i < cap; i++)
        json->fields[i] = xr_null();
    (void) X;
    return json;
}

void xr_json_init_inplace(XrJson *json, XrClass *cls) {
    if (!json || !cls)
        return;
    XR_DCHECK(cls->flags & XR_CLASS_DYNAMIC_LAYOUT, "json_init_inplace: not dynamic-layout");
    json->klass = cls;
    uint16_t cap = cls->in_object_capacity;
    for (uint16_t i = 0; i < cap; i++)
        json->fields[i] = xr_null();
}

size_t xr_json_size(XrClass *cls) {
    return xr_instance_size(cls);
}

/* ========== Field Access API ========== */

XrValue xr_json_get(XrayIsolate *X, XrJson *json, SymbolId symbol) {
    (void) X;
    if (!json || !json->klass)
        return xr_null();
    int idx = xr_class_lookup_field(json->klass, (int) symbol);
    if (idx < 0)
        return xr_null();
    return xr_instance_get_dynamic_field(json, (uint16_t) idx);
}

bool xr_json_set(XrayIsolate *X, XrJson *json, SymbolId symbol, XrValue value) {
    XR_DCHECK(X != NULL, "json_set: NULL isolate");
    if (!json || !json->klass)
        return false;
    int idx = xr_class_lookup_field(json->klass, (int) symbol);
    if (idx < 0) {
        // Field doesn't exist: try transition (returns NULL for sealed/OOM)
        XrSymbolTable *st = get_symbol_table(X);
        const char *fname = xr_symbol_get_name_in_table(st, symbol);
        XrClass *next =
            xr_class_transition_get_or_create(X, json->klass, (int) symbol, fname ? fname : "?");
        if (!next)
            return false;
        json->klass = next;
        idx = xr_class_lookup_field(next, (int) symbol);
        XR_DCHECK(idx >= 0, "json_set: transition produced no field");
    }
    if (!xr_instance_set_dynamic_field(X, json, (uint16_t) idx, value))
        return false;
    XR_GC_BARRIER_BACK_SAFE(xr_current_coro_gc(), json);
    return true;
}

XrValue xr_json_get_by_key(XrayIsolate *X, XrJson *json, const char *key) {
    XR_DCHECK(X != NULL, "json_get_by_key: NULL isolate");
    if (!json || !key)
        return xr_null();
    XrSymbolTable *table = get_symbol_table(X);
    SymbolId symbol = xr_symbol_register_in_table(table, key);
    return xr_json_get(X, json, symbol);
}

bool xr_json_set_by_key(XrayIsolate *X, XrJson *json, const char *key, XrValue value) {
    XR_DCHECK(X != NULL, "json_set_by_key: NULL isolate");
    if (!json || !key)
        return false;
    XrSymbolTable *table = get_symbol_table(X);
    SymbolId symbol = xr_symbol_register_in_table(table, key);
    return xr_json_set(X, json, symbol, value);
}
