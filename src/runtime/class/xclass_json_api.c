/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xclass_json_api.c - Json utility class (static methods only)
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

#include "xclass_json_api.h"
#include "../../base/xchecks.h"
#include "xclass.h"
#include "xclass_builder.h"
#include "xclass_system.h"
#include "../xisolate_api.h"
#include "xreflect_registry.h"
#include "../object/xjson.h"
#include "../object/xmap.h"
#include "../object/xarray.h"
#include "../object/xstring.h"
#include "../../coro/xcoroutine.h"
#include "../symbol/xsymbol_table.h"
#include <string.h>

/* ========== Static Method Implementations ========== */

// Json.keys(obj) -> Array<string>
static XrValue xr_json_static_keys(XrayIsolate *isolate, XrValue *args, int nargs) {
    if (nargs < 1 || !xr_value_is_json(args[0]))
        return xr_value_from_array(xr_array_new(xr_current_coro(isolate)));

    XrJson *json = xr_value_to_json(args[0]);
    if (!json || !xr_json_shape(json))
        return xr_value_from_array(xr_array_new(xr_current_coro(isolate)));

    XrArray *keys = xr_array_new(xr_current_coro(isolate));
    XrSymbolTable *symtab = (XrSymbolTable*)xr_isolate_get_symbol_table(isolate);

    XrShape *shape = xr_json_shape(json);
    for (uint16_t i = 0; i < shape->field_count; i++) {
        SymbolId sym = shape->field_symbols[i];
        const char *name = xr_symbol_get_name_in_table(symtab, sym);
        if (name) {
            xr_array_push(keys, xr_string_value(
                xr_string_intern(isolate, name, strlen(name), 0)));
        }
    }

    return xr_value_from_array(keys);
}

// Json.values(obj) -> Array
static XrValue xr_json_static_values(XrayIsolate *isolate, XrValue *args, int nargs) {
    if (nargs < 1 || !xr_value_is_json(args[0]))
        return xr_value_from_array(xr_array_new(xr_current_coro(isolate)));

    XrJson *json = xr_value_to_json(args[0]);
    if (!json || !xr_json_shape(json))
        return xr_value_from_array(xr_array_new(xr_current_coro(isolate)));

    XrArray *values = xr_array_new(xr_current_coro(isolate));

    XrShape *shape = xr_json_shape(json);
    for (uint16_t i = 0; i < shape->field_count; i++) {
        xr_array_push(values, xr_json_get_field_any(json, i));
    }

    return xr_value_from_array(values);
}

// Json.entries(obj) -> Array<Array<string, any>>
static XrValue xr_json_static_entries(XrayIsolate *isolate, XrValue *args, int nargs) {
    if (nargs < 1 || !xr_value_is_json(args[0]))
        return xr_value_from_array(xr_array_new(xr_current_coro(isolate)));

    XrJson *json = xr_value_to_json(args[0]);
    if (!json || !xr_json_shape(json))
        return xr_value_from_array(xr_array_new(xr_current_coro(isolate)));

    XrArray *entries = xr_array_new(xr_current_coro(isolate));
    XrSymbolTable *symtab = (XrSymbolTable*)xr_isolate_get_symbol_table(isolate);

    XrShape *shape = xr_json_shape(json);
    for (uint16_t i = 0; i < shape->field_count; i++) {
        XrArray *pair = xr_array_new(xr_current_coro(isolate));
        SymbolId sym = shape->field_symbols[i];
        const char *name = xr_symbol_get_name_in_table(symtab, sym);
        if (name) {
            xr_array_push(pair, xr_string_value(
                xr_string_intern(isolate, name, strlen(name), 0)));
        }
        xr_array_push(pair, xr_json_get_field_any(json, i));
        xr_array_push(entries, xr_value_from_array(pair));
    }

    return xr_value_from_array(entries);
}

// Json.has(obj, key) -> bool
static XrValue xr_json_static_has(XrayIsolate *isolate, XrValue *args, int nargs) {
    if (nargs < 2 || !xr_value_is_json(args[0])) return xr_bool(false);

    XrJson *json = xr_value_to_json(args[0]);
    if (!json || !xr_json_shape(json)) return xr_bool(false);
    if (!XR_IS_STRING(args[1])) return xr_bool(false);

    XrString *key_str = XR_TO_STRING(args[1]);
    XrSymbolTable *symtab = (XrSymbolTable*)xr_isolate_get_symbol_table(isolate);
    SymbolId sym = xr_symbol_lookup_in_table(symtab, key_str->data);
    if (sym == SYMBOL_INVALID) return xr_bool(false);

    return xr_bool(xr_json_has_field(json, sym));
}

// Json.get(obj, key, default?) -> any
static XrValue xr_json_static_get(XrayIsolate *isolate, XrValue *args, int nargs) {
    if (nargs < 2) return xr_null();
    if (!xr_value_is_json(args[0])) return (nargs >= 3) ? args[2] : xr_null();

    XrJson *json = xr_value_to_json(args[0]);
    if (!json || !xr_json_shape(json)) return (nargs >= 3) ? args[2] : xr_null();
    if (!XR_IS_STRING(args[1])) return (nargs >= 3) ? args[2] : xr_null();

    XrString *key_str = XR_TO_STRING(args[1]);
    XrSymbolTable *symtab = (XrSymbolTable*)xr_isolate_get_symbol_table(isolate);
    SymbolId sym = xr_symbol_lookup_in_table(symtab, key_str->data);
    if (sym == SYMBOL_INVALID) return (nargs >= 3) ? args[2] : xr_null();

    if (!xr_json_has_field(json, sym)) return (nargs >= 3) ? args[2] : xr_null();
    return xr_json_get_by_key(isolate, json, key_str->data);
}

// Json.size(obj) -> int
static XrValue xr_json_static_size(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void)isolate;
    if (nargs < 1 || !xr_value_is_json(args[0])) return xr_int(0);

    XrJson *json = xr_value_to_json(args[0]);
    if (!json || !xr_json_shape(json)) return xr_int(0);

    return xr_int((xr_Integer)xr_json_field_count(json));
}

// Json.isEmpty(obj) -> bool
static XrValue xr_json_static_isEmpty(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void)isolate;
    if (nargs < 1 || !xr_value_is_json(args[0])) return xr_bool(true);

    XrJson *json = xr_value_to_json(args[0]);
    if (!json || !xr_json_shape(json)) return xr_bool(true);

    return xr_bool(xr_json_field_count(json) == 0);
}

/* ========== Class Creation ========== */

static XrClass* create_json_utility_class(XrayIsolate *X) {
    XR_DCHECK(X != NULL, "create_json_utility_class: NULL isolate");
    XrClassBuilder *builder = xr_class_builder_new(X, "Json", xr_isolate_get_core_classes(X)->objectClass);
    if (!builder) return NULL;

    xr_class_builder_add_static_method(builder, "keys",
        (XrCFunctionPtr)xr_json_static_keys, 1, 0);
    xr_class_builder_add_static_method(builder, "values",
        (XrCFunctionPtr)xr_json_static_values, 1, 0);
    xr_class_builder_add_static_method(builder, "entries",
        (XrCFunctionPtr)xr_json_static_entries, 1, 0);
    xr_class_builder_add_static_method(builder, "has",
        (XrCFunctionPtr)xr_json_static_has, 2, 0);
    xr_class_builder_add_static_method(builder, "get",
        (XrCFunctionPtr)xr_json_static_get, 2, 0);
    xr_class_builder_add_static_method(builder, "size",
        (XrCFunctionPtr)xr_json_static_size, 1, 0);
    xr_class_builder_add_static_method(builder, "isEmpty",
        (XrCFunctionPtr)xr_json_static_isEmpty, 1, 0);

    return xr_class_builder_finalize(builder);
}

/* ========== Initialization ========== */

void xr_json_api_init(XrayIsolate *X) {
    XR_DCHECK(X != NULL, "xr_json_api_init: NULL isolate");
    XrClass *jsonClass = create_json_utility_class(X);

    if (xr_isolate_get_type_registry(X)) {
        xr_registry_register_class(X, jsonClass);
    }

    if (xr_isolate_get_core_classes(X)) {
        xr_isolate_get_core_classes(X)->jsonClass = jsonClass;
    }
}
