/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xjson_methods.c - Json instance method bodies + dispatch table.
 */

#include "xjson_methods.h"
#include "xjson.h"
#include "xarray.h"
#include "xiterator.h"
#include "xstring.h"
#include "../class/xinstance.h"
#include "../value/xvalue.h"
#include "../value/xvalue_format.h"
#include "../symbol/xsymbol_table.h"
#include "../xisolate_api.h"
#include "../../coro/xcoroutine.h"
#include "../../base/xchecks.h"

static inline XrJson *json_self(XrValue self) {
    XR_DCHECK(xr_value_is_json(self), "json method: receiver is not Json");
    return (XrJson *) XR_TO_PTR(self);
}

/* Internal protocol used by `for (k, v in obj)` lowering. */
static XrValue xr_json_method_entries_iterator(XrayIsolate *iso, XrValue self, XrValue *args,
                                               int argc) {
    (void) args;
    (void) argc;
    XrCoroutine *coro = xr_current_coro(iso);
    XrIterator *iter = xr_iterator_new_from_json(coro, json_self(self), iso);
    return iter ? xr_value_from_iterator(iter) : xr_null();
}

/* Single-variable `for (k in jsonObj)` lowering: yields each key string,
 * mirroring Map.iterator() so the two key-value containers behave the
 * same way. */
static XrValue xr_json_method_iterator(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrCoroutine *coro = xr_current_coro(iso);
    XrIterator *iter = xr_iterator_keys_from_json(coro, json_self(self), iso);
    return iter ? xr_value_from_iterator(iter) : xr_null();
}

static XrValue xr_json_method_to_string(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    return xr_string_value(xr_value_to_string(iso, self));
}

/* keys() → Array<string>: return all field names as string array. */
static XrValue xr_json_method_keys(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrCoroutine *coro = xr_current_coro(iso);
    XrArray *result = xr_array_new(coro);
    XR_DCHECK(result != NULL, "json.keys: array alloc failed");

    XrJson *json = json_self(self);
    XrClass *cls = json->klass;
    if (!cls)
        return xr_value_from_array(result);

    for (uint16_t i = 0; i < cls->field_count; i++) {
        const char *name = cls->fields[i].name;
        if (name) {
            XrString *s = xr_string_intern(iso, name, strlen(name), 0);
            xr_array_push(result, xr_string_value(s));
        }
    }
    return xr_value_from_array(result);
}

/* values() → Array<Json>: return all field values. */
static XrValue xr_json_method_values(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrCoroutine *coro = xr_current_coro(iso);
    XrArray *result = xr_array_new(coro);
    XR_DCHECK(result != NULL, "json.values: array alloc failed");

    XrJson *json = json_self(self);
    XrClass *cls = json->klass;
    if (!cls)
        return xr_value_from_array(result);

    for (uint16_t i = 0; i < cls->field_count; i++) {
        xr_array_push(result, xr_instance_get_dynamic_field(json, i));
    }
    return xr_value_from_array(result);
}

/* has(key: string) → bool: check if field exists. */
static XrValue xr_json_method_has(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0]))
        return xr_bool(false);

    XrJson *json = json_self(self);
    XrString *key_str = XR_TO_STRING(args[0]);
    const char *key = XR_STRING_CHARS(key_str);

    XrSymbolTable *symtab = (XrSymbolTable *) xr_isolate_get_symbol_table(iso);
    SymbolId sym = xr_symbol_lookup_in_table(symtab, key);
    if (sym == SYMBOL_INVALID)
        return xr_bool(false);
    return xr_bool(xr_json_has_field(iso, json, sym));
}

/* Type-checking instance methods: isNull(), isInt(), isFloat(), isString(), isBool(),
 * isArray(), isObject().
 *
 * These operate on the *receiver value* (self), NOT on fields. They answer
 * "what JSON value type am I?" which is useful when a Json variable holds
 * a primitive (int, string, etc.) that was coerced into a Json context. */

static XrValue xr_json_method_is_null(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    return xr_bool(XR_IS_NULL(self));
}

static XrValue xr_json_method_is_int(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    return xr_bool(XR_IS_INT(self));
}

static XrValue xr_json_method_is_float(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    return xr_bool(XR_IS_FLOAT(self));
}

static XrValue xr_json_method_is_string(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    return xr_bool(XR_IS_STRING(self));
}

static XrValue xr_json_method_is_bool(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    return xr_bool(XR_IS_BOOL(self));
}

static XrValue xr_json_method_is_array(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    return xr_bool(XR_IS_ARRAY(self));
}

static XrValue xr_json_method_is_object(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    return xr_bool(xr_value_is_json(self));
}

/* ========== XrClass Registration ========== */

#include "../class/xclass_builder.h"

void xr_json_register_instance_methods(XrayIsolate *isolate) {
    XR_DCHECK(isolate != NULL, "json_register_instance_methods: NULL isolate");
    XrayCoreClasses *core = xr_isolate_get_core_classes(isolate);
    XR_DCHECK(core != NULL, "json_register_instance_methods: no core classes");

    // Build a plain XrClass carrying Json instance methods. The class is
    // wired as jsonRootClass->super so dynamic-layout Json instances find
    // these methods via the normal class-chain lookup.
    XrClassBuilder *b = xr_class_builder_new(isolate, "Json", core->objectClass);
    if (!b)
        return;
    xr_class_builder_add_method(b, "iterator", xr_json_method_iterator, 0, 0);
    xr_class_builder_add_method(b, "entriesIterator", xr_json_method_entries_iterator, 0, 0);
    xr_class_builder_add_method(b, "toString", xr_json_method_to_string, 0, 0);
    xr_class_builder_add_method(b, "keys", xr_json_method_keys, 0, 0);
    xr_class_builder_add_method(b, "values", xr_json_method_values, 0, 0);
    xr_class_builder_add_method(b, "has", xr_json_method_has, 1, 0);
    xr_class_builder_add_method(b, "isNull", xr_json_method_is_null, 0, 0);
    xr_class_builder_add_method(b, "isInt", xr_json_method_is_int, 0, 0);
    xr_class_builder_add_method(b, "isFloat", xr_json_method_is_float, 0, 0);
    xr_class_builder_add_method(b, "isString", xr_json_method_is_string, 0, 0);
    xr_class_builder_add_method(b, "isBool", xr_json_method_is_bool, 0, 0);
    xr_class_builder_add_method(b, "isArray", xr_json_method_is_array, 0, 0);
    xr_class_builder_add_method(b, "isObject", xr_json_method_is_object, 0, 0);
    XrClass *cls = xr_class_builder_finalize(b);
    cls->flags |= XR_CLASS_BUILTIN;
    core->jsonInstanceMethodClass = cls;
}
