/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_builtins.c - Builtin method implementations
 *
 * KEY CONCEPT:
 *   Method dispatch and handlers for String/Array/Map/Set/Json.
 *   Uses jump table optimization for method dispatch.
 */

#include "xvm_internal.h"
#include "../base/xchecks.h"
#include "../runtime/gc/xalloc_unified.h"
#include "../runtime/gc/xcoro_gc.h"
#include "../runtime/object/xbigint.h"
#include "../runtime/object/xjson.h"
#include "../runtime/value/xmethod_table.h"
#include "../runtime/xerror_codes.h"
/* DateTime methods used to live here; they now dispatch through
 * native_type_classes (registered by xr_register_native_type) and
 * the per-type method table in stdlib/datetime/datetime_methods.c.
 * Regex / DateTime headers are no longer included on this side of
 * the architecture boundary. The string.match() body that used to
 * pull in stdlib/regex now lives in
 * src/runtime/object/xstring_methods.c (still a reverse include in
 * direction, but contained to a single file pending XrRegex's move
 * into runtime/object/). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* ========== Unified Method Call Dispatch (Jump Table Optimized) ========== */

/* === Map Method Handlers (legacy, kept ONLY for the bound-method
 *     adapter wired via xr_map_get_handler below; the unified
 *     dispatcher in OP_INVOKE_BUILTIN now reaches map methods through
 *     runtime/object/xmap_methods.{c,h} directly). === */

static XrValue map_has_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)isolate;
    if (argc < 1) return xr_bool(0);
    XrMap *map = XR_TO_MAP(receiver);
    return xr_bool(xr_map_has(map, args[0]));
}

static XrValue map_get_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)isolate;
    if (argc < 1) return xr_null();
    XrMap *map = XR_TO_MAP(receiver);
    bool found;
    XrValue result = xr_map_get(map, args[0], &found);
    if (!found && argc >= 2) {
        return args[1];
    }
    return result;
}

static XrValue map_set_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)isolate;
    if (argc >= 2) {
        XrMap *map = XR_TO_MAP(receiver);
        xr_map_set(map, args[0], args[1]);
    }
    return xr_null();
}

static XrValue map_delete_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)isolate;
    if (argc < 1) return xr_bool(0);
    XrMap *map = XR_TO_MAP(receiver);
    return xr_bool(xr_map_delete(map, args[0]));
}

static XrValue map_clear_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)isolate; (void)args; (void)argc;
    XrMap *map = XR_TO_MAP(receiver);
    xr_map_clear(map);
    return xr_null();
}

static XrValue map_keys_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)args; (void)argc;
    XrMap *map = XR_TO_MAP(receiver);
    return xr_value_from_array(xr_map_keys(xr_current_coro(isolate), map));
}

static XrValue map_values_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)args; (void)argc;
    XrMap *map = XR_TO_MAP(receiver);
    return xr_value_from_array(xr_map_values(xr_current_coro(isolate), map));
}

static XrValue map_entries_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)args; (void)argc;
    XrMap *map = XR_TO_MAP(receiver);
    return xr_value_from_array(xr_map_entries(xr_current_coro(isolate), map));
}

static XrValue map_is_empty_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)isolate; (void)args; (void)argc;
    XrMap *map = XR_TO_MAP(receiver);
    return xr_bool(xr_map_is_empty(map));
}

static XrValue map_has_value_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)isolate;
    if (argc < 1) return xr_bool(0);
    XrMap *map = XR_TO_MAP(receiver);
    return xr_bool(xr_map_has_value(map, args[0]));
}

/* Json instance method dispatch lives in
 * src/runtime/object/xjson_methods.{c,h}. The legacy
 * json_method_call_by_symbol used to be here. */


/* String instance methods now live in
 * src/runtime/object/xstring_methods.{c,h}. The legacy
 * string_method_call_by_symbol used to be here. */

/* Array / slice instance methods now live in
 * src/runtime/object/xarray_methods.{c,h}. The legacy
 * array_method_call_by_symbol used to be here. */

/* Set / WeakSet method dispatch lives in
 * src/runtime/object/xset_methods.{c,h}. The legacy
 * set_method_call_by_symbol used to be here; it was deleted when
 * set migrated to the unified XrMethodSlot table. */

/* Numeric and bool method dispatch lives next to the owning value
 * representation:
 *   - bool   -> src/runtime/value/xbool_methods.{c,h}
 *   - int    -> src/runtime/value/xint_methods.{c,h}
 *   - float  -> src/runtime/value/xfloat_methods.{c,h}
 *   - bigint -> src/runtime/object/xbigint_methods.{c,h}
 *
 * Each module exports `xr_<type>_method_table[]` and registers it in
 * runtime/value/xmethod_table.c. The legacy *_method_call_by_symbol
 * dispatchers used to live here and were deleted as the migration
 * sweep landed; OP_INVOKE_BUILTIN now resolves through the unified
 * XrMethodSlot table for these types. */

// Bound method value helpers now live in runtime/closure/xbound_method.c.

/* DateTime instance methods now live in
 * stdlib/datetime/datetime_methods.{c,h} as a per-type XrMethodSlot
 * table. Actual VM dispatch for DateTime continues to flow through
 * native_type_classes (registered at module load by
 * xr_register_native_type) — the new table is staged for AOT
 * codegen and the upcoming invoke-IC integration so DateTime
 * methods become compile-time-foldable like the runtime types. */

/* ========== BoundMethod Standalone Handlers ========== */

// Iterator hasNext/next
#include "../runtime/object/xiterator.h"

static XrValue iterator_hasnext_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)isolate; (void)args; (void)argc;
    XrIterator *iter = xr_value_to_iterator(receiver);
    return xr_bool(xr_iterator_has_next(iter));
}

static XrValue iterator_next_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)isolate; (void)args; (void)argc;
    XrIterator *iter = xr_value_to_iterator(receiver);
    return xr_iterator_next(iter);
}

// Stub for unimplemented bound method calls (returns xr_null)
static XrValue bound_method_stub(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)isolate; (void)receiver; (void)args; (void)argc;
    return xr_null();
}

/* ========== BoundMethod Handler Lookup (for zero-dispatch calls) ========== */

MethodHandler xr_map_get_handler(int symbol) {
    if (symbol == SYMBOL_HAS) return map_has_handler;
    if (symbol == SYMBOL_GET) return map_get_handler;
    if (symbol == SYMBOL_SET) return map_set_handler;
    if (symbol == SYMBOL_DELETE) return map_delete_handler;
    if (symbol == SYMBOL_CLEAR) return map_clear_handler;
    if (symbol == SYMBOL_KEYS) return map_keys_handler;
    if (symbol == SYMBOL_VALUES) return map_values_handler;
    if (symbol == SYMBOL_ENTRIES) return map_entries_handler;
    if (symbol == SYMBOL_IS_EMPTY) return map_is_empty_handler;
    if (symbol == SYMBOL_HAS_VALUE_MAP) return map_has_value_handler;
    if (symbol == SYMBOL_FOREACH) return bound_method_stub;
    return NULL;
}

MethodHandler xr_array_get_handler(int symbol) {
    /* Pull from the unified xr_array_method_table — same XrMethodFn
     * signature, no duplicate registry. Closure-taking methods
     * (foreach/map/filter/reduce/find/...) resolve through the
     * bound-method stub when the user grabs them as values. */
    const XrMethodSlot *slot = xr_method_table_lookup(
        XR_TID_ARRAY, symbol, SYMBOL_BUILTIN_COUNT);
    if (slot) return (MethodHandler)slot->fn;
    if (symbol == SYMBOL_ITERATOR) return bound_method_stub;
    return NULL;
}

MethodHandler xr_set_get_handler(int symbol) {
    /* Bound-method dispatch reuses the unified XrMethodFn signature
     * defined in runtime/value/xmethod_table.h. Look the slot up
     * once and return its fn; foreach / map / filter do not have
     * direct handlers and resolve through the closure adapter. */
    const XrMethodSlot *slot = xr_method_table_lookup(
        XR_TID_SET, symbol, SYMBOL_BUILTIN_COUNT);
    if (slot) return (MethodHandler)slot->fn;
    if (symbol == SYMBOL_FOREACH) return bound_method_stub;
    if (symbol == SYMBOL_MAP_METHOD) return bound_method_stub;
    if (symbol == SYMBOL_FILTER) return bound_method_stub;
    return NULL;
}

MethodHandler xr_string_get_handler(int symbol) {
    /* Pull from the unified xr_string_method_table — same XrMethodFn
     * signature, no duplicate registry. SYMBOL_ITERATOR remains a
     * bound-method stub: the lazy character iterator hasn't been
     * lifted into a table entry yet. */
    const XrMethodSlot *slot = xr_method_table_lookup(
        XR_TID_STRING, symbol, SYMBOL_BUILTIN_COUNT);
    if (slot) return (MethodHandler)slot->fn;
    if (symbol == SYMBOL_ITERATOR) return bound_method_stub;
    return NULL;
}

MethodHandler xr_iterator_get_handler(int symbol) {
    if (symbol == SYMBOL_HASNEXT) return iterator_hasnext_handler;
    if (symbol == SYMBOL_NEXT) return iterator_next_handler;
    return NULL;
}

// Enum getMember handler (uniform MethodHandler signature)
XrValue xr_enum_get_member_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)isolate;
    if (argc < 1 || !XR_IS_INT(args[0])) return xr_null();
    if (!XR_IS_PTR(receiver)) return xr_null();

    XrGCHeader *gc = (XrGCHeader*)XR_TO_PTR(receiver);
    if (XR_GC_GET_TYPE(gc) != XR_TENUM_TYPE) return xr_null();

    XrEnumType *enum_type = (XrEnumType*)gc;
    int index = XR_TO_INT(args[0]);

    if (index < 0 || index >= (int)enum_type->member_count) {
        return xr_null();
    }

    return XR_FROM_PTR(enum_type->members[index].instance);
}
