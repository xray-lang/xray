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
#include "xshape.h"
#include "xstring.h"
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
    XrShape *shape = xr_json_shape(iso, json);
    if (!shape)
        return xr_value_from_array(result);

    XrSymbolTable *symtab = (XrSymbolTable *) xr_isolate_get_symbol_table(iso);
    for (uint16_t i = 0; i < shape->field_count; i++) {
        SymbolId sym = shape->field_symbols[i];
        const char *name = xr_symbol_get_name_in_table(symtab, sym);
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
    XrShape *shape = xr_json_shape(iso, json);
    if (!shape)
        return xr_value_from_array(result);

    for (uint16_t i = 0; i < shape->field_count; i++) {
        xr_array_push(result, xr_json_get_field_any(iso, json, i));
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

const XrMethodSlot xr_json_method_table[SYMBOL_BUILTIN_COUNT] = {
    [SYMBOL_ENTRIES_ITERATOR] = {xr_json_method_entries_iterator, 0, 0, 0},
    [SYMBOL_TOSTRING] = {xr_json_method_to_string, 0, 0, XR_METHOD_FLAG_MAY_THROW},
    [SYMBOL_KEYS] = {xr_json_method_keys, 0, 0, 0},
    [SYMBOL_VALUES] = {xr_json_method_values, 0, 0, 0},
    [SYMBOL_HAS] = {xr_json_method_has, 1, 1, 0},
    [SYMBOL_IS_NULL] = {xr_json_method_is_null, 0, 0, 0},
    [SYMBOL_IS_INT] = {xr_json_method_is_int, 0, 0, 0},
    [SYMBOL_IS_FLOAT] = {xr_json_method_is_float, 0, 0, 0},
    [SYMBOL_IS_STRING] = {xr_json_method_is_string, 0, 0, 0},
    [SYMBOL_IS_BOOL] = {xr_json_method_is_bool, 0, 0, 0},
    [SYMBOL_IS_ARRAY] = {xr_json_method_is_array, 0, 0, 0},
    [SYMBOL_IS_OBJECT] = {xr_json_method_is_object, 0, 0, 0},
};

/* ========== XrClass Registration ========== */

#include "xnative_type.h"

void xr_json_register_native_type(XrayIsolate *isolate) {
    static const XrNativeMethod json_methods[] = {
        {"entriesIterator", xr_json_method_entries_iterator, 0},
        {"toString", xr_json_method_to_string, 0},
        {"keys", xr_json_method_keys, 0},
        {"values", xr_json_method_values, 0},
        {"has", xr_json_method_has, 1},
        {"isNull", xr_json_method_is_null, 0},
        {"isInt", xr_json_method_is_int, 0},
        {"isFloat", xr_json_method_is_float, 0},
        {"isString", xr_json_method_is_string, 0},
        {"isBool", xr_json_method_is_bool, 0},
        {"isArray", xr_json_method_is_array, 0},
        {"isObject", xr_json_method_is_object, 0},
        {NULL, NULL, 0},
    };
    static const XrNativeTypeInfo json_info = {
        .name = "Json",
        .gc_type = XR_TJSON,
        .methods = json_methods,
        .getters = NULL,
        .static_methods = NULL,
    };
    xr_register_native_type(isolate, &json_info);
}
