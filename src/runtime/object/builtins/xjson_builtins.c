/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xjson_builtins.c - Json utility class (static methods only)
 *
 * KEY CONCEPT:
 *   Json objects have no instance methods to avoid name conflicts
 *   with user-defined fields. All operations go through Json.xxx()
 *   static methods. This eliminates the conflict between builtin
 *   method names (has, delete, keys, etc.) and user field names.
 *
 * WHY THIS DESIGN:
 *   - Json is an open-property data container, any property name
 *     could be user data, so instance methods would conflict.
 *   - Static methods (Json.keys(obj)) keep the `.` namespace
 *     entirely for user data.
 */

#include "xjson_builtins.h"
#include "../xjson_serde.h"
#include "xchecks.h"
#include "xclass.h"
#include "xclass_builder.h"
#include "xclass_system.h"
#include "xisolate_api.h"
#include "xreflect_registry.h"
#include "../xjson.h"
#include "../xmap.h"
#include "../xtuple.h"
#include "../xarray.h"
#include "../xstring.h"
#include "../../coro/xcoroutine.h"
#include "../../symbol/xsymbol_table.h"
#include "../xexception.h"
#include "../../vm/xvm.h"
#include <string.h>

/* ========== Static Method Implementations ========== */

// Json.keys(obj) -> Array<string>
static XrValue xr_json_static_keys(XrayIsolate *isolate, XrValue self, XrValue *args, int nargs) {
    (void) self;
    if (nargs < 1 || !xr_value_is_json(args[0]))
        return xr_value_from_array(xr_array_new(xr_current_coro(isolate)));

    XrJson *json = xr_value_to_json(args[0]);
    if (!json || !json->klass)
        return xr_value_from_array(xr_array_new(xr_current_coro(isolate)));

    XrArray *keys = xr_array_new(xr_current_coro(isolate));
    XrClass *cls = json->klass;
    for (uint16_t i = 0; i < cls->field_count; i++) {
        const char *name = cls->fields[i].name;
        if (name) {
            xr_array_push(keys, xr_string_value(xr_string_intern(isolate, name, strlen(name), 0)));
        }
    }

    return xr_value_from_array(keys);
}

// Json.values(obj) -> Array
static XrValue xr_json_static_values(XrayIsolate *isolate, XrValue self, XrValue *args, int nargs) {
    (void) self;
    if (nargs < 1 || !xr_value_is_json(args[0]))
        return xr_value_from_array(xr_array_new(xr_current_coro(isolate)));

    XrJson *json = xr_value_to_json(args[0]);
    if (!json || !json->klass)
        return xr_value_from_array(xr_array_new(xr_current_coro(isolate)));

    XrArray *values = xr_array_new(xr_current_coro(isolate));
    XrClass *cls = json->klass;
    for (uint16_t i = 0; i < cls->field_count; i++) {
        xr_array_push(values, xr_instance_get_dynamic_field(json, i));
    }

    return xr_value_from_array(values);
}

// Json.entries(obj) -> Array<(string, Json)>
static XrValue xr_json_static_entries(XrayIsolate *isolate, XrValue self, XrValue *args,
                                      int nargs) {
    (void) self;
    if (nargs < 1 || !xr_value_is_json(args[0]))
        return xr_value_from_array(xr_array_new(xr_current_coro(isolate)));

    XrJson *json = xr_value_to_json(args[0]);
    if (!json || !json->klass)
        return xr_value_from_array(xr_array_new(xr_current_coro(isolate)));

    XrArray *entries = xr_array_new(xr_current_coro(isolate));
    XrCoroutine *coro = xr_current_coro(isolate);
    XrClass *cls = json->klass;
    for (uint16_t i = 0; i < cls->field_count; i++) {
        /* Each entry is a (key, value) tuple: heterogeneous arity-2
         * product that destructures cleanly in user code via
         * `for ((k, v) in Json.entries(obj))`. */
        XrTuple *pair = xr_tuple_new(coro, 2);
        if (pair) {
            const char *name = cls->fields[i].name;
            XrValue key_v = name ? xr_string_value(xr_string_intern(isolate, name, strlen(name), 0))
                                 : xr_string_value(xr_string_intern(isolate, "", 0, 0));
            xr_tuple_set(pair, 0, key_v);
            xr_tuple_set(pair, 1, xr_instance_get_dynamic_field(json, i));
        }
        xr_array_push(entries, xr_value_from_tuple(pair));
    }

    return xr_value_from_array(entries);
}

// Json.has(obj, key) -> bool
static XrValue xr_json_static_has(XrayIsolate *isolate, XrValue self, XrValue *args, int nargs) {
    (void) self;
    if (nargs < 2 || !xr_value_is_json(args[0]))
        return xr_bool(false);

    XrJson *json = xr_value_to_json(args[0]);
    if (!json || !json->klass)
        return xr_bool(false);
    if (!XR_IS_STRING(args[1]))
        return xr_bool(false);

    XrString *key_str = XR_TO_STRING(args[1]);
    XrSymbolTable *symtab = (XrSymbolTable *) xr_isolate_get_symbol_table(isolate);
    SymbolId sym = xr_symbol_lookup_in_table(symtab, key_str->data);
    if (sym == SYMBOL_INVALID)
        return xr_bool(false);

    return xr_bool(xr_json_has_field(isolate, json, sym));
}

// Json.get(obj, key, default?) -> any
static XrValue xr_json_static_get(XrayIsolate *isolate, XrValue self, XrValue *args, int nargs) {
    (void) self;
    if (nargs < 2)
        return xr_null();
    if (!xr_value_is_json(args[0]))
        return (nargs >= 3) ? args[2] : xr_null();

    XrJson *json = xr_value_to_json(args[0]);
    if (!json || !json->klass)
        return (nargs >= 3) ? args[2] : xr_null();
    if (!XR_IS_STRING(args[1]))
        return (nargs >= 3) ? args[2] : xr_null();

    XrString *key_str = XR_TO_STRING(args[1]);
    XrSymbolTable *symtab = (XrSymbolTable *) xr_isolate_get_symbol_table(isolate);
    SymbolId sym = xr_symbol_lookup_in_table(symtab, key_str->data);
    if (sym == SYMBOL_INVALID)
        return (nargs >= 3) ? args[2] : xr_null();

    if (!xr_json_has_field(isolate, json, sym))
        return (nargs >= 3) ? args[2] : xr_null();
    return xr_json_get_by_key(isolate, json, key_str->data);
}

// Json.size(obj) -> int
static XrValue xr_json_static_size(XrayIsolate *isolate, XrValue self, XrValue *args, int nargs) {
    (void) self;
    if (nargs < 1 || !xr_value_is_json(args[0]))
        return xr_int(0);

    XrJson *json = xr_value_to_json(args[0]);
    if (!json || !json->klass)
        return xr_int(0);

    return xr_int((xr_Integer) xr_json_field_count(isolate, json));
}

// Json.isEmpty(obj) -> bool
static XrValue xr_json_static_isEmpty(XrayIsolate *isolate, XrValue self, XrValue *args,
                                      int nargs) {
    (void) self;
    if (nargs < 1 || !xr_value_is_json(args[0]))
        return xr_bool(true);

    XrJson *json = xr_value_to_json(args[0]);
    if (!json || !json->klass)
        return xr_bool(true);

    return xr_bool(xr_json_field_count(isolate, json) == 0);
}

// Json.stringify(value, indent?) — thin wrapper that calls the core
// stringify engine and throws a TypeError on non-serializable types.
static XrValue xr_json_builtin_stringify(XrayIsolate *X, XrValue self, XrValue *args, int argc) {
    (void) self;
    if (argc < 1)
        return xr_null();

    int indent = 0;
    if (argc >= 2 && XR_IS_INT(args[1])) {
        indent = (int) XR_TO_INT(args[1]);
    }

    XrJsonStringifyResult r = xr_json_stringify_core(X, args[0], indent);
    if (r.has_error) {
        XrValue exc = xr_exception_newf(X, XR_ERR_JSON_INVALID, "Json.stringify: %s", r.error_msg);
        xr_vm_unwind_with_trace(X, exc);
        return xr_null();
    }
    return r.result;
}

/* ========== Class Creation ========== */

static XrClass *create_json_utility_class(XrayIsolate *X) {
    XR_DCHECK(X != NULL, "create_json_utility_class: NULL isolate");
    XrClassBuilder *builder =
        xr_class_builder_new(X, "Json", xr_isolate_get_core_classes(X)->objectClass);
    if (!builder)
        return NULL;

    xr_class_builder_add_static_method(builder, "keys", xr_json_static_keys, 1, 0);
    xr_class_builder_add_static_method(builder, "values", xr_json_static_values, 1, 0);
    xr_class_builder_add_static_method(builder, "entries", xr_json_static_entries, 1, 0);
    xr_class_builder_add_static_method(builder, "has", xr_json_static_has, 2, 0);
    xr_class_builder_add_static_method(builder, "get", xr_json_static_get, 2, 0);
    xr_class_builder_add_static_method(builder, "size", xr_json_static_size, 1, 0);
    xr_class_builder_add_static_method(builder, "isEmpty", xr_json_static_isEmpty, 1, 0);

    // JSON parse/stringify — core engine in xjson_serde.c, throw wrapper above
    xr_class_builder_add_static_method(builder, "parse", xr_json_fn_parse, 1, 0);
    xr_class_builder_add_static_method(builder, "stringify", xr_json_builtin_stringify, 2, 0);
    xr_class_builder_add_static_method(builder, "isValid", xr_json_fn_is_valid, 1, 0);
    xr_class_builder_add_static_method(builder, "tryParse", xr_json_fn_try_parse, 1, 0);

    return xr_class_builder_finalize(builder);
}

/* ========== Initialization ========== */

void xr_json_api_init(XrayIsolate *X) {
    XR_DCHECK(X != NULL, "xr_json_api_init: NULL isolate");
    // create_json_utility_class goes through xr_class_builder_finalize,
    // which already registers the resulting class with the reflection
    // type registry. No manual registration is required here.
    XrClass *jsonClass = create_json_utility_class(X);

    if (xr_isolate_get_core_classes(X)) {
        xr_isolate_get_core_classes(X)->jsonClass = jsonClass;
    }
}
