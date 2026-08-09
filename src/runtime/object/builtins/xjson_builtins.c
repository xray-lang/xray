/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xjson_builtins.c - JSON namespace class (static methods only)
 *
 * KEY CONCEPT:
 *   JSON object values have no instance methods to avoid name conflicts
 *   with user-defined fields. All operations go through JSON.xxx()
 *   static methods. This eliminates the conflict between builtin
 *   method names (has, delete, keys, etc.) and user field names.
 *
 * WHY THIS DESIGN:
 *   - Json is an open-property data container, any property name
 *     could be user data, so instance methods would conflict.
 *   - Static methods (JSON.keys(obj)) keep the `.` namespace
 *     entirely for user data.
 */

#include "xjson_builtins.h"
#include "../xjson_serde.h"
#include "xchecks.h"
#include "xclass.h"
#include "xclass_builder.h"
#include "xclass_system.h"
#include "xisolate_api.h"
#include "xtype_registry.h"
#include "../xjson.h"
#include "../xmap.h"
#include "../xtuple.h"
#include "../xarray.h"
#include "../xstring.h"
#include "../../coro/xcoroutine.h"
#include "../../symbol/xsymbol_table.h"
#include "../xpanic_info.h"
#include "../../vm/xvm.h"
#include <string.h>

/* ========== Static Method Implementations ========== */

typedef enum XrJsonRuntimeKind {
    XR_JSON_RUNTIME_INVALID = -1,
    XR_JSON_RUNTIME_NULL,
    XR_JSON_RUNTIME_BOOL,
    XR_JSON_RUNTIME_INT,
    XR_JSON_RUNTIME_FLOAT,
    XR_JSON_RUNTIME_STRING,
    XR_JSON_RUNTIME_ARRAY,
    XR_JSON_RUNTIME_OBJECT,
} XrJsonRuntimeKind;

static XrJsonRuntimeKind xr_json_runtime_kind(XrValue value) {
    if (XR_IS_NULL(value))
        return XR_JSON_RUNTIME_NULL;
    if (XR_IS_BOOL(value))
        return XR_JSON_RUNTIME_BOOL;
    if (XR_IS_INT(value))
        return XR_JSON_RUNTIME_INT;
    if (XR_IS_FLOAT(value))
        return XR_JSON_RUNTIME_FLOAT;
    if (XR_IS_STRING(value))
        return XR_JSON_RUNTIME_STRING;
    if (XR_IS_ARRAY(value))
        return XR_JSON_RUNTIME_ARRAY;
    if (XR_IS_MAP(value))
        return XR_JSON_RUNTIME_OBJECT;
    return XR_JSON_RUNTIME_INVALID;
}

static XrValue xr_json_static_kind_of(XrVMRuntime *X, XrValue self, XrValue *args, int nargs) {
    (void) self;
    static const char *const names[] = {"null",   "bool",  "int",   "float",
                                        "string", "array", "object"};
    XrJsonRuntimeKind kind = nargs >= 1 ? xr_json_runtime_kind(args[0]) : XR_JSON_RUNTIME_INVALID;
    XR_DCHECK(kind != XR_JSON_RUNTIME_INVALID,
              "JSON.kindOf: value is outside the JSON.Value domain");
    if (kind == XR_JSON_RUNTIME_INVALID)
        return xr_null();
    const char *name = names[kind];
    return xr_string_value(xr_string_intern(X, name, strlen(name), 0));
}

#define XR_JSON_KIND_PREDICATE(fn_name, expected_kind)                                             \
    static XrValue fn_name(XrVMRuntime *X, XrValue self, XrValue *args, int nargs) {               \
        (void) X;                                                                                  \
        (void) self;                                                                               \
        return xr_bool(nargs >= 1 && xr_json_runtime_kind(args[0]) == (expected_kind));            \
    }

XR_JSON_KIND_PREDICATE(xr_json_static_is_null, XR_JSON_RUNTIME_NULL)
XR_JSON_KIND_PREDICATE(xr_json_static_is_bool, XR_JSON_RUNTIME_BOOL)
XR_JSON_KIND_PREDICATE(xr_json_static_is_int, XR_JSON_RUNTIME_INT)
XR_JSON_KIND_PREDICATE(xr_json_static_is_float, XR_JSON_RUNTIME_FLOAT)
XR_JSON_KIND_PREDICATE(xr_json_static_is_string, XR_JSON_RUNTIME_STRING)
XR_JSON_KIND_PREDICATE(xr_json_static_is_array, XR_JSON_RUNTIME_ARRAY)
XR_JSON_KIND_PREDICATE(xr_json_static_is_object, XR_JSON_RUNTIME_OBJECT)

#undef XR_JSON_KIND_PREDICATE

// JSON.stringify(value, indent?) — thin wrapper that calls the core
// stringify engine and throws a TypeError on non-serializable types.
static XrValue xr_json_builtin_stringify(XrVMRuntime *X, XrValue self, XrValue *args, int argc) {
    (void) self;
    if (argc < 1)
        return xr_null();

    int indent = 0;
    if (argc >= 2 && XR_IS_INT(args[1])) {
        indent = (int) XR_TO_INT(args[1]);
    }

    XrJsonStringifyResult r = xr_json_stringify_core(X, args[0], indent);
    if (r.has_error) {
        XrValue exc = xr_panic_info_newf(X, XR_ERR_JSON_INVALID, "JSON.stringify: %s", r.error_msg);
        xr_vm_unwind_with_trace(X, exc);
        return xr_null();
    }
    return r.result;
}

// JSON.value(value) — explicit typed-value -> JSON.Value boundary. This uses the
// serde encoder directly so the hot path avoids stringify/parse round-trips.
static XrValue xr_json_builtin_encode(XrVMRuntime *X, XrValue self, XrValue *args, int argc) {
    (void) self;
    if (argc < 1)
        return xr_null();

    XrJsonEncodeResult r = xr_json_encode_core(X, args[0]);
    if (r.has_error) {
        XrValue exc = xr_panic_info_newf(X, XR_ERR_JSON_INVALID, "JSON.value: %s", r.error_msg);
        xr_vm_unwind_with_trace(X, exc);
        return xr_null();
    }
    return r.result;
}

static XrValue xr_json_builtin_merge_with_rest(XrVMRuntime *X, XrValue self, XrValue *args,
                                               int argc) {
    (void) self;
    XrObjectInstance *parts = argc >= 1 ? xr_value_to_object_instance(args[0]) : NULL;
    XrValue rest_value = parts ? xr_object_instance_get_by_key(X, parts, "rest") : xr_null();
    XrValue typed_value = parts ? xr_object_instance_get_by_key(X, parts, "value") : xr_null();
    if (!parts || !XR_IS_MAP(rest_value)) {
        XrValue exc =
            xr_panic_info_newf(X, XR_ERR_JSON_INVALID, "JSON.merge: expected JSON.WithRest<T>");
        xr_vm_unwind_with_trace(X, exc);
        return xr_null();
    }

    XrJsonEncodeResult encoded = xr_json_encode_core(X, typed_value);
    if (encoded.has_error || !XR_IS_MAP(encoded.result)) {
        XrValue exc = xr_panic_info_newf(
            X, XR_ERR_JSON_INVALID, "JSON.merge: %s",
            encoded.has_error ? encoded.error_msg : "typed value did not encode as an object");
        xr_vm_unwind_with_trace(X, exc);
        return xr_null();
    }

    XrMap *dst = XR_TO_MAP(encoded.result);
    XrMap *rest = XR_TO_MAP(rest_value);
    for (uint32_t i = 0; rest && i < rest->nentries; i++) {
        XrMapEntry *entry = xr_map_entry(rest, i);
        if (entry->key_tt == XR_MAP_ENTRY_NIL_KEY)
            continue;
        if (!XR_IS_STRING(entry->key) || xr_map_has(dst, entry->key)) {
            xr_rc_release_value(xr_current_coro_heap(), encoded.result);
            XrValue exc = xr_panic_info_newf(
                X, XR_ERR_JSON_INVALID,
                XR_IS_STRING(entry->key)
                    ? "JSON.merge: rest conflicts with a declared top-level field"
                    : "JSON.merge: rest contains a non-string key");
            xr_vm_unwind_with_trace(X, exc);
            return xr_null();
        }
        xr_rc_retain_value(entry->key);
        xr_rc_retain_value(entry->value);
        xr_map_set(dst, entry->key, entry->value);
    }
    return encoded.result;
}

static XrValue xr_json_builtin_parse_value(XrVMRuntime *X, XrValue self, XrValue *args, int argc) {
    (void) self;
    XrJsonParseResult parsed = xr_json_parse_core(X, argc >= 1 ? args[0] : xr_null());
    if (!parsed.has_error)
        return parsed.result;

    XrValue exc = xr_panic_info_newf(X, XR_ERR_JSON_INVALID, "JSON.parse: invalid JSON");
    xr_vm_unwind_with_trace(X, exc);
    return xr_null();
}

static XrValue xr_json_builtin_parse_object(XrVMRuntime *X, XrValue self, XrValue *args, int argc) {
    (void) self;
    XrJsonParseResult parsed = xr_json_parse_core(X, argc >= 1 ? args[0] : xr_null());
    if (parsed.has_error) {
        XrValue exc = xr_panic_info_newf(X, XR_ERR_JSON_INVALID, "JSON.parseObject: invalid JSON");
        xr_vm_unwind_with_trace(X, exc);
        return xr_null();
    }
    if (XR_IS_MAP(parsed.result))
        return parsed.result;
    XrValue exc = xr_panic_info_newf(X, XR_ERR_JSON_INVALID,
                                     "JSON.parseObject: root value must be an object");
    xr_vm_unwind_with_trace(X, exc);
    return xr_null();
}

static XrValue xr_json_builtin_as_object(XrVMRuntime *X, XrValue self, XrValue *args, int argc) {
    (void) X;
    (void) self;
    XrValue result = argc >= 1 && XR_IS_MAP(args[0]) ? args[0] : xr_null();
    xr_rc_retain_value(result);
    return result;
}

static XrValue xr_json_builtin_as_array(XrVMRuntime *X, XrValue self, XrValue *args, int argc) {
    (void) X;
    (void) self;
    XrValue result = argc >= 1 && XR_IS_ARRAY(args[0]) ? args[0] : xr_null();
    xr_rc_retain_value(result);
    return result;
}

static bool xr_json_path_read(XrValue root, XrArray *path, XrValue *out) {
    if (!path || !out)
        return false;
    XrValue current = root;
    for (int i = 0; i < path->length; i++) {
        XrValue segment = xr_array_get(path, i);
        if (XR_IS_STRING(segment) && XR_IS_MAP(current)) {
            bool found = false;
            current = xr_map_get(XR_TO_MAP(current), segment, &found);
            if (!found)
                return false;
            continue;
        }
        if (XR_IS_INT(segment) && XR_IS_ARRAY(current)) {
            int64_t index = XR_TO_INT(segment);
            XrArray *array = XR_TO_ARRAY(current);
            if (index < 0 || index >= array->length)
                return false;
            current = xr_array_get(array, (int) index);
            continue;
        }
        return false;
    }
    *out = current;
    return true;
}

static XrValue xr_json_builtin_get_path(XrVMRuntime *X, XrValue self, XrValue *args, int argc) {
    (void) X;
    (void) self;
    XrValue result = xr_null();
    if (argc < 2 || !XR_IS_ARRAY(args[1]))
        return result;
    if (!xr_json_path_read(args[0], XR_TO_ARRAY(args[1]), &result))
        return xr_null();
    xr_rc_retain_value(result);
    return result;
}

static XrValue xr_json_builtin_require_path(XrVMRuntime *X, XrValue self, XrValue *args, int argc) {
    (void) self;
    XrValue result = xr_null();
    if (argc >= 2 && XR_IS_ARRAY(args[1]) &&
        xr_json_path_read(args[0], XR_TO_ARRAY(args[1]), &result)) {
        xr_rc_retain_value(result);
        return result;
    }
    XrValue exc = xr_panic_info_newf(X, XR_ERR_JSON_INVALID,
                                     "JSON.require: path does not exist or crosses a wrong kind");
    xr_vm_unwind_with_trace(X, exc);
    return xr_null();
}

static XrValue xr_json_builtin_contains_path(XrVMRuntime *X, XrValue self, XrValue *args,
                                             int argc) {
    (void) X;
    (void) self;
    XrValue ignored = xr_null();
    return xr_bool(argc >= 2 && XR_IS_ARRAY(args[1]) &&
                   xr_json_path_read(args[0], XR_TO_ARRAY(args[1]), &ignored));
}

static bool xr_json_path_parent(XrValue root, XrArray *path, bool create_parents,
                                XrValue *out_parent, XrValue *out_last) {
    if (!path || path->length == 0 || !out_parent || !out_last)
        return false;
    XrValue current = root;
    for (int i = 0; i + 1 < path->length; i++) {
        XrValue segment = xr_array_get(path, i);
        XrValue next_segment = xr_array_get(path, i + 1);
        if (XR_IS_STRING(segment) && XR_IS_MAP(current)) {
            XrMap *map = XR_TO_MAP(current);
            bool found = false;
            XrValue next = xr_map_get(map, segment, &found);
            if (!found && create_parents && XR_IS_STRING(next_segment)) {
                XrMap *child = xr_map_new(NULL);
                if (!child)
                    return false;
                xr_rc_retain_value(segment);
                next = xr_value_from_map(child);
                xr_map_set(map, segment, next);
                found = true;
            }
            if (!found)
                return false;
            current = next;
            continue;
        }
        if (XR_IS_INT(segment) && XR_IS_ARRAY(current)) {
            int64_t index = XR_TO_INT(segment);
            XrArray *array = XR_TO_ARRAY(current);
            if (index < 0 || index >= array->length)
                return false;
            current = xr_array_get(array, (int) index);
            continue;
        }
        return false;
    }
    *out_parent = current;
    *out_last = xr_array_get(path, path->length - 1);
    return true;
}

static XrValue xr_json_builtin_set_path(XrVMRuntime *X, XrValue self, XrValue *args, int argc) {
    (void) self;
    bool create_parents = argc >= 4 && XR_IS_BOOL(args[3]) && XR_TO_BOOL(args[3]);
    XrValue parent = xr_null();
    XrValue last = xr_null();
    if (argc >= 3 && XR_IS_ARRAY(args[1]) &&
        xr_json_path_parent(args[0], XR_TO_ARRAY(args[1]), create_parents, &parent, &last)) {
        if (XR_IS_STRING(last) && XR_IS_MAP(parent)) {
            xr_rc_retain_value(last);
            xr_rc_retain_value(args[2]);
            xr_map_set(XR_TO_MAP(parent), last, args[2]);
            return xr_null();
        }
        if (XR_IS_INT(last) && XR_IS_ARRAY(parent)) {
            int64_t index = XR_TO_INT(last);
            XrArray *array = XR_TO_ARRAY(parent);
            if (index >= 0 && index < array->length) {
                xr_array_set(array, (int) index, args[2]);
                return xr_null();
            }
        }
    }
    XrValue exc = xr_panic_info_newf(X, XR_ERR_JSON_INVALID,
                                     "JSON.set: path does not exist or crosses a wrong kind");
    xr_vm_unwind_with_trace(X, exc);
    return xr_null();
}

static bool xr_json_array_remove(XrArray *array, int64_t index) {
    if (!array || index < 0 || index >= array->length)
        return false;
    for (int64_t i = index; i + 1 < array->length; i++)
        xr_array_set(array, (int) i, xr_array_get(array, (int) i + 1));
    XrValue removed = xr_array_pop(array);
    xr_rc_release_value(xr_current_coro_heap(), removed);
    return true;
}

static XrValue xr_json_builtin_remove_path(XrVMRuntime *X, XrValue self, XrValue *args, int argc) {
    (void) self;
    XrValue parent = xr_null();
    XrValue last = xr_null();
    if (argc >= 2 && XR_IS_ARRAY(args[1]) &&
        xr_json_path_parent(args[0], XR_TO_ARRAY(args[1]), false, &parent, &last)) {
        if (XR_IS_STRING(last) && XR_IS_MAP(parent))
            return xr_bool(xr_map_delete(XR_TO_MAP(parent), last));
        if (XR_IS_INT(last) && XR_IS_ARRAY(parent))
            return xr_bool(xr_json_array_remove(XR_TO_ARRAY(parent), XR_TO_INT(last)));
    }
    XrValue exc =
        xr_panic_info_newf(X, XR_ERR_JSON_INVALID, "JSON.remove: path crosses a wrong kind");
    xr_vm_unwind_with_trace(X, exc);
    return xr_bool(false);
}

/* ========== Class Creation ========== */

static XrClass *create_json_utility_class(XrVMRuntime *X) {
    XR_DCHECK(X != NULL, "create_json_utility_class: NULL isolate");
    XrClassBuilder *builder =
        xr_class_builder_new(X, "JSON", xr_isolate_get_core_classes(X)->objectClass);
    if (!builder)
        return NULL;

    xr_class_builder_add_static_method(builder, "kindOf", xr_json_static_kind_of, 1, 0);
    xr_class_builder_add_static_method(builder, "isNull", xr_json_static_is_null, 1, 0);
    xr_class_builder_add_static_method(builder, "isBool", xr_json_static_is_bool, 1, 0);
    xr_class_builder_add_static_method(builder, "isInt", xr_json_static_is_int, 1, 0);
    xr_class_builder_add_static_method(builder, "isFloat", xr_json_static_is_float, 1, 0);
    xr_class_builder_add_static_method(builder, "isString", xr_json_static_is_string, 1, 0);
    xr_class_builder_add_static_method(builder, "isArray", xr_json_static_is_array, 1, 0);
    xr_class_builder_add_static_method(builder, "isObject", xr_json_static_is_object, 1, 0);

    // JSON parse/stringify — core engine in xjson_serde.c, throw wrapper above
    xr_class_builder_add_static_method(builder, "parse", xr_json_builtin_parse_value, 1, 0);
    xr_class_builder_add_static_method(builder, "parseValue", xr_json_builtin_parse_value, 1, 0);
    xr_class_builder_add_static_method(builder, "parseObject", xr_json_builtin_parse_object, 1, 0);
    xr_class_builder_add_static_method(builder, "stringify", xr_json_builtin_stringify, 2, 0);
    xr_class_builder_add_static_method(builder, "value", xr_json_builtin_encode, 1, 0);
    xr_class_builder_add_static_method(builder, "asObject", xr_json_builtin_as_object, 1, 0);
    xr_class_builder_add_static_method(builder, "asArray", xr_json_builtin_as_array, 1, 0);
    xr_class_builder_add_static_method(builder, "get", xr_json_builtin_get_path, 2, 0);
    xr_class_builder_add_static_method(builder, "require", xr_json_builtin_require_path, 2, 0);
    xr_class_builder_add_static_method(builder, "containsPath", xr_json_builtin_contains_path, 2,
                                       0);
    xr_class_builder_add_static_method(builder, "set", xr_json_builtin_set_path, 4, 0);
    xr_class_builder_add_static_method(builder, "remove", xr_json_builtin_remove_path, 2, 0);
    xr_class_builder_add_static_method(builder, "merge", xr_json_builtin_merge_with_rest, 1, 0);
    xr_class_builder_add_static_method(builder, "isValid", xr_json_fn_is_valid, 2, 0);

    return xr_class_builder_finalize(builder);
}

/* ========== Initialization ========== */

void xr_json_api_init(XrVMRuntime *X) {
    XR_DCHECK(X != NULL, "xr_json_api_init: NULL isolate");
    // create_json_utility_class goes through xr_class_builder_finalize,
    // which already registers the resulting class with the type registry
    // type registry. No manual registration is required here.
    XrClass *jsonClass = create_json_utility_class(X);

    if (xr_isolate_get_core_classes(X)) {
        xr_isolate_get_core_classes(X)->jsonClass = jsonClass;
    }
}
