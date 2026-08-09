/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xjson.c - Exact structural object backed by sealed XrInstance layouts
 *
 * Shape classes are interned through the structural-object root. JSON object
 * values are stored separately as XrMap.
 */

#include "xjson.h"
#include "../../base/xchecks.h"
#include "../mem/xheap.h"
#include "../mem/xalloc_unified.h"
#include "../core/xr_exec_context.h"
#include "../../base/xmalloc.h"
#include "../class/xinstance.h"
#include "../coro/xcoroutine.h"
#include "../xisolate_api.h"
#include "../xisolate_internal.h"
#include "../symbol/xsymbol_table.h"
#include <string.h>
#include <stdio.h>

static inline XrSymbolTable *get_symbol_table(XrVMRuntime *isolate) {
    return (XrSymbolTable *) xr_isolate_get_symbol_table(isolate);
}

/* ========== Creation API ========== */

XrObjectInstance *xr_object_instance_new_with_class(struct XrCoroutine *coro, XrClass *cls) {
    XR_DCHECK(cls != NULL, "object_instance_new_with_class: NULL class");
    XR_DCHECK((cls->flags & XR_CLASS_DYNAMIC_LAYOUT) != 0,
              "object_instance_new_with_class: not shape-backed");
    XR_DCHECK((cls->flags & XR_CLASS_DYNAMIC_SEALED) != 0,
              "object_instance_new_with_class: shape is not sealed");
    XR_DCHECK(cls->builtin_kind == XR_BK_STRUCT_OBJECT,
              "object_instance_new_with_class: not a structural object class");
    size_t size = xr_instance_size(cls);
    XrObjectInstance *json = (XrObjectInstance *) xr_alloc(coro, size, XR_TINSTANCE);
    if (!json)
        return NULL;
    xr_obj_header_init_type(&json->hdr, XR_TINSTANCE);
    json->klass = cls;
    uint16_t cap = cls->in_object_capacity;
    for (uint16_t i = 0; i < cap; i++)
        json->fields[i] = xr_null();
    return json;
}

void xr_object_instance_init_inplace(XrObjectInstance *json, XrClass *cls) {
    if (!json || !cls)
        return;
    XR_DCHECK((cls->flags & XR_CLASS_DYNAMIC_LAYOUT) != 0 &&
                  (cls->flags & XR_CLASS_DYNAMIC_SEALED) != 0 &&
                  cls->builtin_kind == XR_BK_STRUCT_OBJECT,
              "object_instance_init_inplace: invalid structural shape");
    json->klass = cls;
    uint16_t cap = cls->in_object_capacity;
    for (uint16_t i = 0; i < cap; i++)
        json->fields[i] = xr_null();
}

size_t xr_object_instance_size(XrClass *cls) {
    return xr_instance_size(cls);
}

/* ========== Field Access API ========== */

XrValue xr_object_instance_get(XrVMRuntime *X, XrObjectInstance *json, SymbolId symbol) {
    (void) X;
    if (!json || !json->klass)
        return xr_null();
    int idx = xr_class_lookup_field(json->klass, (int) symbol);
    if (idx < 0)
        return xr_null();
    return xr_instance_get_dynamic_field(json, (uint16_t) idx);
}

bool xr_object_instance_set(XrVMRuntime *X, XrObjectInstance *json, SymbolId symbol,
                            XrValue value) {
    XR_DCHECK(X != NULL, "object_instance_set: NULL isolate");
    if (!json || !json->klass)
        return false;
    int idx = xr_class_lookup_field(json->klass, (int) symbol);
    if (idx < 0)
        return false;
    if (!xr_instance_set_dynamic_field(X, json, (uint16_t) idx, value))
        return false;
    return true;
}

XrValue xr_object_instance_get_by_key(XrVMRuntime *X, XrObjectInstance *json, const char *key) {
    XR_DCHECK(X != NULL, "json_get_by_key: NULL isolate");
    if (!json || !key)
        return xr_null();
    XrSymbolTable *table = get_symbol_table(X);
    SymbolId symbol = xr_symbol_register_in_table(table, key);
    return xr_object_instance_get(X, json, symbol);
}

bool xr_object_instance_set_by_key(XrVMRuntime *X, XrObjectInstance *json, const char *key,
                                   XrValue value) {
    XR_DCHECK(X != NULL, "json_set_by_key: NULL isolate");
    if (!json || !key)
        return false;
    XrSymbolTable *table = get_symbol_table(X);
    SymbolId symbol = xr_symbol_register_in_table(table, key);
    return xr_object_instance_set(X, json, symbol, value);
}

bool xr_object_instance_merge(XrVMRuntime *X, XrObjectInstance *dst, XrObjectInstance *src) {
    XR_DCHECK(X != NULL, "json_merge: NULL isolate");
    if (!dst || !src || !src->klass)
        return true;
    XrClass *cls = src->klass;
    uint16_t n = cls->field_count;
    for (uint16_t i = 0; i < n; i++) {
        const char *name = cls->fields[i].name;
        if (!name)
            continue;
        // Source field is borrowed: dst gains a new owning reference, src keeps
        // its own. xr_object_instance_set releases any prior value at this key, so later
        // spread parts / literal fields override earlier ones correctly.
        XrValue v = xr_instance_get_dynamic_field(src, i);
        xr_rc_retain_value(v);
        if (!xr_object_instance_set_by_key(X, dst, name, v)) {
            xr_rc_release_value(xr_current_coro_heap(), v);
            return false;
        }
    }
    return true;
}
