/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xjson_serde.c - JSON serialization/deserialization engine
 *
 * KEY CONCEPT:
 *   RFC 8259 compliant JSON parser and serializer. Handles escape sequences,
 *   Unicode (\uXXXX), and proper type conversion between JSON and xray values.
 *   Enum values serialize as member name strings; DateTime as ISO 8601.
 *   Non-serializable types (function, class, channel) cause stringify to
 *   return an error result; the caller (xjson_builtins.c) decides whether
 *   to throw. This keeps the serde layer free of VM dependencies.
 */

#include "xjson_serde.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include "../../base/xmalloc.h"
#include "../../base/xsimd.h"
#include "../../base/xutf8.h"
#include "../../base/xjson.h"

#include "xmap.h"
#include "xset.h"
#include "xarray.h"
#include "../class/xinstance.h"
#include "../class/xclass.h"
#include "../class/xenum.h"
#include "xjson.h"
#include "../symbol/xsymbol_table.h"
#include "../xisolate_internal.h"
#include "../xstdlib_bridge.h"
#include "../value/xtype_names.h"

/* Forward-declare XrDateTime — definition lives in stdlib/datetime/datetime.h
 * but this L2 module must not depend on stdlib. We only need the pointer type
 * and one formatting function (implemented in stdlib/datetime/datetime.c,
 * linked into the same binary). */
typedef struct XrDateTime XrDateTime;
XR_FUNC int xr_datetime_to_iso_string(XrDateTime *dt, char *buf, size_t buf_size);

/* ========== JSON Parser (delegates to src/base/xjson) ========== */

#define JSON_MAX_DEPTH 256

/*
 * Convert a pure-C XrJsonValue DOM tree (from src/base/xjson) to runtime
 * XrValue objects (XrString, XrArray, XrJson). This is the bridge layer
 * that avoids duplicating parser code in the stdlib.
 *
 * Number heuristic: JSON has no integer type; we return xr_int when the
 * double has no fractional part and fits in int64, else xr_float.
 */
static XrValue dom_to_xrvalue(XrVMRuntime *X, XrJsonValue *v) {
    if (!v)
        return xr_null();

    switch (v->type) {
        case XR_JSON_NULL:
            return xr_null();

        case XR_JSON_BOOL:
            return xr_bool(v->as.boolean);

        case XR_JSON_NUMBER:
            if (v->is_integer) {
                return xr_int(v->as.integer);
            }
            return xr_float(v->as.number);

        case XR_JSON_STRING: {
            size_t len = v->string_len;
            XrString *str = xr_string_intern(X, v->as.string, len, 0);
            return xr_string_value(str);
        }

        case XR_JSON_ARRAY: {
            XrArray *arr = xr_array_new(xr_current_coro(X));
            for (int idx = 0; idx < v->as.array.count; idx++) {
                xr_array_push(arr, dom_to_xrvalue(X, v->as.array.items[idx]));
            }
            return xr_value_from_array(arr);
        }

        case XR_JSON_OBJECT: {
            XrJson *json = xr_json_new(xr_current_coro(X));
            if (!json)
                return xr_null();
            for (int idx = 0; idx < v->as.object.count; idx++) {
                xr_json_set_by_key(X, json, v->as.object.members[idx].key,
                                   dom_to_xrvalue(X, v->as.object.members[idx].value));
            }
            return xr_json_value(json);
        }
    }
    return xr_null();
}

/* ========== JSON Serialization ========== */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    XrVMRuntime *isolate;
    int indent;
    int depth;
    bool has_error;
    char error_msg[128];
    // Container pointers on the current serialization path, for precise cycle
    // detection (matches JS JSON.stringify throwing on circular structures).
    const void *seen[JSON_MAX_DEPTH];
    int seen_count;
} JsonWriter;

typedef struct {
    XrVMRuntime *isolate;
    int depth;
    bool has_error;
    char error_msg[128];
} JsonEncoder;

static inline void writer_init(JsonWriter *w, XrVMRuntime *isolate, int indent) {
    w->cap = 1024;
    w->data = (char *) xr_malloc(w->cap);
    XR_CHECK(w->data != NULL, "JsonWriter: allocation failed");
    w->len = 0;
    w->isolate = isolate;
    w->indent = indent;
    w->depth = 0;
    w->has_error = false;
    w->error_msg[0] = '\0';
    w->seen_count = 0;
}

static inline void writer_free(JsonWriter *w) {
    if (w->data) {
        xr_free(w->data);
        w->data = NULL;
    }
}

static inline void writer_grow(JsonWriter *w, size_t extra) {
    size_t needed = w->len + extra + 1;
    if (needed <= w->cap)
        return;
    size_t new_cap = w->cap * 2;
    if (new_cap < needed)
        new_cap = needed;
    char *tmp = (char *) xr_realloc(w->data, new_cap);
    XR_CHECK(tmp != NULL, "JsonWriter: realloc failed");
    w->data = tmp;
    w->cap = new_cap;
}

static inline void writer_append(JsonWriter *w, const char *s, size_t n) {
    writer_grow(w, n);
    memcpy(w->data + w->len, s, n);
    w->len += n;
}

static inline void writer_char(JsonWriter *w, char c) {
    writer_grow(w, 1);
    w->data[w->len++] = c;
}

static inline void writer_str(JsonWriter *w, const char *s) {
    writer_append(w, s, strlen(s));
}

static inline void writer_newline(JsonWriter *w) {
    if (w->indent <= 0)
        return;
    writer_char(w, '\n');
    int n = w->depth * w->indent;
    for (int i = 0; i < n; i++)
        writer_char(w, ' ');
}

// Forward declaration
static void stringify_value(JsonWriter *w, XrValue val);

static void encode_value(JsonEncoder *e, XrValue val, XrValue *out);

static inline void encode_error(JsonEncoder *e, const char *msg) {
    if (!e->has_error) {
        e->has_error = true;
        snprintf(e->error_msg, sizeof(e->error_msg), "%s", msg);
    }
}

static inline void encode_type_error(JsonEncoder *e, XrValue val) {
    if (!e->has_error) {
        XrTypeId tid = xr_value_typeid(val);
        snprintf(e->error_msg, sizeof(e->error_msg), "cannot encode value of type '%s' to JSON",
                 xr_typeid_name(tid));
        e->has_error = true;
    }
}

// Stringify string: batch non-escape spans for fewer writer calls
static void stringify_string(JsonWriter *w, const char *s, size_t len) {
    writer_char(w, '"');
    size_t i = 0;
    while (i < len) {
        // Scan ahead for characters that don't need escaping
        size_t start = i;
        while (i < len) {
            unsigned char c = s[i];
            if (c < 32 || c == '"' || c == '\\')
                break;
            i++;
        }
        // Flush non-escape span in one call
        if (i > start) {
            writer_append(w, s + start, i - start);
        }
        if (i >= len)
            break;
        // Handle escape character
        unsigned char c = s[i];
        switch (c) {
            case '"':
                writer_append(w, "\\\"", 2);
                break;
            case '\\':
                writer_append(w, "\\\\", 2);
                break;
            case '\n':
                writer_append(w, "\\n", 2);
                break;
            case '\r':
                writer_append(w, "\\r", 2);
                break;
            case '\t':
                writer_append(w, "\\t", 2);
                break;
            case '\b':
                writer_append(w, "\\b", 2);
                break;
            case '\f':
                writer_append(w, "\\f", 2);
                break;
            default: {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                writer_append(w, buf, 6);
                break;
            }
        }
        i++;
    }
    writer_char(w, '"');
}

// Stringify array
static void stringify_array(JsonWriter *w, XrArray *arr) {
    writer_char(w, '[');

    int count = arr->length;
    if (count > 0) {
        w->depth++;
        for (int i = 0; i < count; i++) {
            if (i > 0)
                writer_char(w, ',');
            writer_newline(w);
            stringify_value(w, xr_array_get(arr, i));
            if (w->has_error)
                break;
        }
        w->depth--;
        writer_newline(w);
    }

    writer_char(w, ']');
}

// Stringify Map.
//
// NOTE: ordering. XrMap is a chained hash table (xmap.h) and does not
// track insertion order. We iterate node slots in index order, which
// is deterministic for a given map instance but unrelated to the order
// entries were inserted. Scripts that need insertion-preserving
// round-trip should build their object via XrJson (shape-backed, which
// `stringify_json` preserves exactly).
static void stringify_map(JsonWriter *w, XrMap *map) {
    writer_char(w, '{');

    size_t output_count = 0;
    w->depth++;

    // Get node count
    uint32_t size = (map->flags & XR_MAP_FLAG_DUMMY) ? 0 : map->nentries;

    for (uint32_t i = 0; i < size; i++) {
        XrMapEntry *node = xr_map_entry(map, i);
        if (node->key_tt == 0)
            continue;

        // Keys must be JSON-representable. Strings are emitted
        // verbatim; integers are stringified (acceptable per common
        // JSON-as-config usage). Any other key type (float, array,
        // map, instance) is skipped — previously we emitted the
        // placeholder `"<key>"`, which silently collides on read-back
        // and is worse than losing the entry.
        char intkey_buf[32];
        const char *key_ptr = NULL;
        size_t key_len = 0;
        if (XR_IS_STRING(node->key)) {
            XrString *key = XR_TO_STRING(node->key);
            key_ptr = key->data;
            key_len = key->length;
        } else if (XR_IS_INT(node->key)) {
            int n =
                snprintf(intkey_buf, sizeof(intkey_buf), "%lld", (long long) XR_TO_INT(node->key));
            if (n > 0) {
                key_ptr = intkey_buf;
                key_len = (size_t) n;
            }
        } else {
            continue;  // skip entries with non-stringifiable keys
        }

        if (output_count > 0)
            writer_char(w, ',');
        writer_newline(w);

        stringify_string(w, key_ptr, key_len);
        writer_char(w, ':');
        if (w->indent > 0)
            writer_char(w, ' ');

        stringify_value(w, node->value);
        output_count++;
        if (w->has_error)
            break;
    }

    w->depth--;
    if (output_count > 0)
        writer_newline(w);
    writer_char(w, '}');
}

// Stringify XrJson object
static void stringify_json(JsonWriter *w, XrJson *json) {
    writer_char(w, '{');

    if (!json || !json->klass) {
        writer_char(w, '}');
        return;
    }

    w->depth++;
    size_t output_count = 0;

    XrClass *cls = json->klass;
    for (uint16_t i = 0; i < cls->field_count; i++) {
        if (output_count > 0)
            writer_char(w, ',');
        writer_newline(w);

        const char *name = cls->fields[i].name;
        if (name) {
            stringify_string(w, name, strlen(name));
        } else {
            char buf[16];
            snprintf(buf, sizeof(buf), "field%d", i);
            stringify_string(w, buf, strlen(buf));
        }

        writer_char(w, ':');
        if (w->indent > 0)
            writer_char(w, ' ');
        stringify_value(w, xr_instance_get_dynamic_field(json, i));
        output_count++;
        if (w->has_error)
            break;
    }

    w->depth--;
    if (output_count > 0)
        writer_newline(w);
    writer_char(w, '}');
}

// Stringify Instance (Struct/Class instance)
static void stringify_instance(JsonWriter *w, XrInstance *inst) {
    writer_char(w, '{');

    XrClass *cls = xr_instance_get_class(inst);
    if (!cls) {
        writer_char(w, '}');
        return;
    }

    w->depth++;
    size_t output_count = 0;

    // Iterate over all fields (including inherited)
    for (uint16_t i = 0; i < cls->field_count; i++) {
        // Skip static fields
        if (cls->fields[i].flags & XR_FIELD_STATIC)
            continue;

        if (output_count > 0)
            writer_char(w, ',');
        writer_newline(w);

        // Field name
        const char *name = cls->fields[i].name;
        if (name) {
            stringify_string(w, name, strlen(name));
        } else {
            char buf[16];
            snprintf(buf, sizeof(buf), "field%d", i);
            stringify_string(w, buf, strlen(buf));
        }

        writer_char(w, ':');
        if (w->indent > 0)
            writer_char(w, ' ');

        // Field value (access by index)
        XrValue field_val = xr_instance_get_field_fast(inst, i);
        stringify_value(w, field_val);
        output_count++;
        if (w->has_error)
            break;
    }

    w->depth--;
    if (output_count > 0)
        writer_newline(w);
    writer_char(w, '}');
}

// Push a container pointer onto the serialization path. Returns false (and
// flags the writer error) if `ptr` is already on the path (circular structure)
// or the path is too deep — never silently truncates to "null", which would
// corrupt data unnoticed (P1-1).
static bool stringify_enter(JsonWriter *w, const void *ptr) {
    for (int i = 0; i < w->seen_count; i++) {
        if (w->seen[i] == ptr) {
            if (!w->has_error) {
                w->has_error = true;
                snprintf(w->error_msg, sizeof(w->error_msg),
                         "cannot serialize circular structure to JSON");
            }
            return false;
        }
    }
    if (w->seen_count >= JSON_MAX_DEPTH) {
        if (!w->has_error) {
            w->has_error = true;
            snprintf(w->error_msg, sizeof(w->error_msg),
                     "cannot serialize structure nested deeper than %d levels to JSON",
                     JSON_MAX_DEPTH);
        }
        return false;
    }
    w->seen[w->seen_count++] = ptr;
    return true;
}

static inline void stringify_leave(JsonWriter *w) {
    if (w->seen_count > 0)
        w->seen_count--;
}

// Stringify value. Container types are guarded by stringify_enter/leave, which
// detect circular references and excessive nesting and flag an error instead
// of silently emitting "null".
static void stringify_value(JsonWriter *w, XrValue val) {
    if (w->has_error)
        return;
    if (XR_IS_NULL(val)) {
        writer_str(w, "null");
    } else if (XR_IS_BOOL(val)) {
        writer_str(w, XR_TO_BOOL(val) ? "true" : "false");
    } else if (XR_IS_INT(val)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", (long long) XR_TO_INT(val));
        writer_str(w, buf);
    } else if (XR_IS_FLOAT(val)) {
        double d = XR_TO_FLOAT(val);
        char buf[32];
        if (isinf(d) || isnan(d)) {
            writer_str(w, "null");  // JSON doesn't support Infinity/NaN
        } else {
            // Shortest round-trip: try %.15g first (DBL_DIG); only
            // fall back to %.17g if the shorter form doesn't round-trip.
            snprintf(buf, sizeof(buf), "%.15g", d);
            if (strtod(buf, NULL) != d)
                snprintf(buf, sizeof(buf), "%.17g", d);
            writer_str(w, buf);
        }
    } else if (XR_IS_STRING(val)) {
        XrString *s = XR_TO_STRING(val);
        stringify_string(w, s->data, s->length);
    } else if (XR_IS_CHAR(val)) {
        char buf[XR_UTF8_MAX_BYTES];
        int n = xr_utf8_encode(XR_TO_CHAR(val), buf);
        if (n > 0)
            stringify_string(w, buf, (size_t) n);
        else
            writer_str(w, "null");
    } else if (XR_IS_ARRAY(val)) {
        XrArray *arr = XR_TO_ARRAY(val);
        if (stringify_enter(w, arr)) {
            stringify_array(w, arr);
            stringify_leave(w);
        }
    } else if (xr_value_is_json(val)) {
        XrJson *json = xr_value_to_json(val);
        if (stringify_enter(w, json)) {
            stringify_json(w, json);
            stringify_leave(w);
        }
    } else if (XR_IS_MAP(val)) {
        XrMap *map = XR_TO_MAP(val);
        if (stringify_enter(w, map)) {
            stringify_map(w, map);
            stringify_leave(w);
        }
    } else if (XR_IS_ENUM_VALUE(val)) {
        // Enum value: serialize as member name string
        XrEnumValue *ev = XR_TO_ENUM_VALUE(val);
        const char *name = ev->member_name ? ev->member_name : "null";
        if (ev->member_name)
            stringify_string(w, name, strlen(name));
        else
            writer_str(w, "null");
    } else if (xr_value_is_datetime(w->isolate, val)) {
        // DateTime: serialize as ISO 8601 string. Body lives at the
        // native-body offset inside the XrInstance, accessed via
        // xr_instance_native_body (cast through void* keeps this
        // module decoupled from stdlib/datetime headers).
        XrInstance *inst = (XrInstance *) XR_TO_PTR(val);
        XrDateTime *dt = (XrDateTime *) xr_instance_native_body(inst);
        char buf[64];
        int n = xr_datetime_to_iso_string(dt, buf, sizeof(buf));
        if (n > 0)
            stringify_string(w, buf, (size_t) n);
        else
            writer_str(w, "null");
    } else if (xr_value_is_instance(val)) {
        // Struct or Class instance
        XrInstance *inst = (XrInstance *) XR_TO_PTR(val);
        if (stringify_enter(w, inst)) {
            stringify_instance(w, inst);
            stringify_leave(w);
        }
    } else {
        // Non-serializable type: record error
        if (!w->has_error) {
            w->has_error = true;
            XrTypeId tid = xr_value_typeid(val);
            snprintf(w->error_msg, sizeof(w->error_msg),
                     "cannot serialize value of type '%s' to JSON", xr_typeid_name(tid));
        }
        writer_str(w, "null");
    }
}

static void encode_array(JsonEncoder *e, XrArray *arr, XrValue *out) {
    XrArray *copy = xr_array_with_capacity(xr_current_coro(e->isolate), arr ? arr->length : 0);
    if (!copy) {
        encode_error(e, "out of memory while encoding Array to JSON");
        *out = xr_null();
        return;
    }

    int count = arr ? arr->length : 0;
    for (int i = 0; i < count && !e->has_error; i++) {
        XrValue encoded = xr_null();
        encode_value(e, xr_array_get(arr, i), &encoded);
        if (!e->has_error)
            xr_array_push(copy, encoded);
    }

    *out = e->has_error ? xr_null() : xr_value_from_array(copy);
}

static void encode_object_fields(JsonEncoder *e, XrInstance *inst, bool dynamic_fields,
                                 XrValue *out) {
    XrJson *json = xr_json_new(xr_current_coro(e->isolate));
    if (!json) {
        encode_error(e, "out of memory while encoding object to JSON");
        *out = xr_null();
        return;
    }

    XrClass *cls = xr_instance_get_class(inst);
    if (!cls) {
        *out = xr_json_value(json);
        return;
    }

    for (uint16_t i = 0; i < cls->field_count && !e->has_error; i++) {
        if (cls->fields[i].flags & XR_FIELD_STATIC)
            continue;
        const char *name = cls->fields[i].name;
        if (!name)
            continue;

        XrValue field = dynamic_fields ? xr_instance_get_dynamic_field(inst, i)
                                       : xr_instance_get_field_fast(inst, i);
        XrValue encoded = xr_null();
        encode_value(e, field, &encoded);
        if (!e->has_error)
            xr_json_set_by_key(e->isolate, json, name, encoded);
    }

    *out = e->has_error ? xr_null() : xr_json_value(json);
}

static void encode_map(JsonEncoder *e, XrMap *map, XrValue *out) {
    XrJson *json = xr_json_new(xr_current_coro(e->isolate));
    if (!json) {
        encode_error(e, "out of memory while encoding Map to JSON");
        *out = xr_null();
        return;
    }
    if (!map || (map->flags & XR_MAP_FLAG_DUMMY)) {
        *out = xr_json_value(json);
        return;
    }

    for (uint32_t i = 0; i < map->nentries && !e->has_error; i++) {
        XrMapEntry *entry = xr_map_entry(map, i);
        if (entry->key_tt == XR_MAP_ENTRY_NIL_KEY)
            continue;

        char int_key[32];
        const char *key = NULL;
        if (XR_IS_STRING(entry->key)) {
            key = XR_TO_STRING(entry->key)->data;
        } else if (XR_IS_INT(entry->key)) {
            snprintf(int_key, sizeof(int_key), "%lld", (long long) XR_TO_INT(entry->key));
            key = int_key;
        } else {
            encode_error(e, "Json.encode(Map) requires string or int keys");
            break;
        }

        XrValue encoded = xr_null();
        encode_value(e, entry->value, &encoded);
        if (!e->has_error)
            xr_json_set_by_key(e->isolate, json, key, encoded);
    }

    *out = e->has_error ? xr_null() : xr_json_value(json);
}

static void encode_set(JsonEncoder *e, XrSet *set, XrValue *out) {
    XrArray *arr = xr_array_with_capacity(xr_current_coro(e->isolate), set ? (int) set->count : 0);
    if (!arr) {
        encode_error(e, "out of memory while encoding Set to JSON");
        *out = xr_null();
        return;
    }
    if (!set || (set->flags & XR_SET_FLAG_DUMMY)) {
        *out = xr_value_from_array(arr);
        return;
    }

    for (uint32_t i = 0; i < set->nentries && !e->has_error; i++) {
        XrSetEntry *entry = xr_set_entry(set, i);
        if (entry->val_tt == XR_SET_ENTRY_NIL)
            continue;
        XrValue encoded = xr_null();
        encode_value(e, entry->value, &encoded);
        if (!e->has_error)
            xr_array_push(arr, encoded);
    }

    *out = e->has_error ? xr_null() : xr_value_from_array(arr);
}

static void encode_value(JsonEncoder *e, XrValue val, XrValue *out) {
    if (e->depth >= JSON_MAX_DEPTH) {
        encode_error(e, "value is too deeply nested for JSON");
        *out = xr_null();
        return;
    }

    if (XR_IS_NULL(val) || XR_IS_BOOL(val) || XR_IS_INT(val) || XR_IS_STRING(val)) {
        *out = val;
        return;
    }

    if (XR_IS_FLOAT(val)) {
        double d = XR_TO_FLOAT(val);
        *out = (isinf(d) || isnan(d)) ? xr_null() : val;
        return;
    }

    if (XR_IS_CHAR(val)) {
        char buf[XR_UTF8_MAX_BYTES];
        int n = xr_utf8_encode(XR_TO_CHAR(val), buf);
        *out = n > 0 ? xr_string_value(xr_string_new(e->isolate, buf, (size_t) n)) : xr_null();
        return;
    }

    e->depth++;
    if (XR_IS_ARRAY(val)) {
        encode_array(e, XR_TO_ARRAY(val), out);
    } else if (xr_value_is_json(val) || xr_value_is_record(val)) {
        encode_object_fields(e, (XrInstance *) XR_TO_PTR(val), true, out);
    } else if (XR_IS_MAP(val)) {
        encode_map(e, XR_TO_MAP(val), out);
    } else if (XR_IS_SET(val)) {
        encode_set(e, XR_TO_SET(val), out);
    } else if (XR_IS_ENUM_VALUE(val)) {
        XrEnumValue *ev = XR_TO_ENUM_VALUE(val);
        const char *name = ev->member_name ? ev->member_name : "";
        *out = xr_string_value(xr_string_intern(e->isolate, name, strlen(name), 0));
    } else if (xr_value_is_datetime(e->isolate, val)) {
        XrInstance *inst = (XrInstance *) XR_TO_PTR(val);
        XrDateTime *dt = (XrDateTime *) xr_instance_native_body(inst);
        char buf[64];
        int n = xr_datetime_to_iso_string(dt, buf, sizeof(buf));
        *out = n > 0 ? xr_string_value(xr_string_new(e->isolate, buf, (size_t) n)) : xr_null();
    } else if (xr_value_is_instance(val)) {
        encode_object_fields(e, (XrInstance *) XR_TO_PTR(val), false, out);
    } else {
        encode_type_error(e, val);
        *out = xr_null();
    }
    e->depth--;
}

/* ========== Public Functions ========== */

// parse(str) - Parse JSON string
XrValue xr_json_fn_parse(XrVMRuntime *X, XrValue self, XrValue *args, int argc) {
    (void) self;
    if (argc < 1 || !XR_IS_STRING(args[0])) {
        return xr_null();
    }

    XrString *str = XR_TO_STRING(args[0]);
    XrJsonValue *dom = xjson_parse(str->data, str->length);
    if (!dom)
        return xr_null();

    XrValue result = dom_to_xrvalue(X, dom);
    xjson_free(dom);
    return result;
}

// Core stringify: serialize value to string, return result + error info.
// Does NOT throw — the caller decides how to handle errors.
XrJsonStringifyResult xr_json_stringify_core(XrVMRuntime *X, XrValue val, int indent) {
    XrJsonStringifyResult out = {.result = xr_null(), .has_error = false};
    out.error_msg[0] = '\0';

    if (indent < 0)
        indent = 0;
    if (indent > 8)
        indent = 8;

    JsonWriter writer;
    writer_init(&writer, X, indent);

    stringify_value(&writer, val);

    if (writer.has_error) {
        out.has_error = true;
        memcpy(out.error_msg, writer.error_msg, sizeof(out.error_msg));
        writer_free(&writer);
        return out;
    }

    XrString *str = xr_string_new(X, writer.data, writer.len);
    writer_free(&writer);

    out.result = xr_string_value(str);
    return out;
}

XrJsonEncodeResult xr_json_encode_core(XrVMRuntime *X, XrValue val) {
    XrJsonEncodeResult out = {.result = xr_null(), .has_error = false};
    out.error_msg[0] = '\0';

    JsonEncoder enc = {.isolate = X, .depth = 0, .has_error = false};
    enc.error_msg[0] = '\0';
    encode_value(&enc, val, &out.result);
    if (enc.has_error) {
        out.has_error = true;
        memcpy(out.error_msg, enc.error_msg, sizeof(out.error_msg));
        out.result = xr_null();
    }
    return out;
}

// Serialize XrValue to JSON C-string.
// Caller MUST release the returned pointer with xr_free() (not free()) so
// that the debug allocator tracks the deallocation correctly.
char *xr_json_stringify_to_cstr(XrVMRuntime *X, XrValue val, size_t *out_len) {
    JsonWriter writer;
    writer_init(&writer, X, 0);
    stringify_value(&writer, val);

    // Null-terminate
    writer_char(&writer, '\0');
    if (out_len)
        *out_len = writer.len - 1;  // exclude null terminator

    char *result = writer.data;
    writer.data = NULL;  // prevent writer_free from freeing
    return result;
}

// Parse JSON C-string to XrValue
// Returns xr_null() on parse error. Input need not be null-terminated if len is provided.
XrValue xr_json_parse_from_cstr(XrVMRuntime *X, const char *json_str, size_t len) {
    if (!X || !json_str || len == 0)
        return xr_null();

    XrJsonValue *dom = xjson_parse(json_str, len);
    if (!dom)
        return xr_null();

    XrValue result = dom_to_xrvalue(X, dom);
    xjson_free(dom);
    return result;
}

/* ========== Lightweight JSON Validator (zero GC allocation) ========== */

typedef struct {
    const char *ptr;
    int depth;
    bool ok;
    bool strict;  // RFC 8259 strict mode: reject bare control chars (< 0x20)
} JsonValidator;

static void validate_skip_ws(JsonValidator *v) {
    while (*v->ptr && isspace((unsigned char) *v->ptr))
        v->ptr++;
}

static void validate_value(JsonValidator *v);

static void validate_string(JsonValidator *v) {
    if (*v->ptr != '"') {
        v->ok = false;
        return;
    }
    v->ptr++;
    while (*v->ptr && *v->ptr != '"') {
        unsigned char c = (unsigned char) *v->ptr;
        // RFC 8259 §7: unescaped bytes <= 0x1F inside a string are
        // forbidden. Under strict flag we reject them; the non-strict
        // path stays permissive for existing callers.
        if (v->strict && c < 0x20) {
            v->ok = false;
            return;
        }
        if (*v->ptr == '\\') {
            v->ptr++;
            if (!*v->ptr) {
                v->ok = false;
                return;
            }
            if (*v->ptr == 'u') {
                for (int i = 0; i < 4; i++) {
                    v->ptr++;
                    if (!isxdigit((unsigned char) *v->ptr)) {
                        v->ok = false;
                        return;
                    }
                }
            } else {
                const char *valid = "\"\\/bfnrt";
                if (!strchr(valid, *v->ptr)) {
                    v->ok = false;
                    return;
                }
            }
        }
        v->ptr++;
    }
    if (*v->ptr != '"') {
        v->ok = false;
        return;
    }
    v->ptr++;
}

static void validate_number(JsonValidator *v) {
    if (*v->ptr == '-')
        v->ptr++;
    if (!isdigit((unsigned char) *v->ptr)) {
        v->ok = false;
        return;
    }
    const char *dstart = v->ptr;
    while (isdigit((unsigned char) *v->ptr))
        v->ptr++;
    if ((v->ptr - dstart) > 1 && *dstart == '0') {
        v->ok = false;
        return;
    }
    if (*v->ptr == '.') {
        v->ptr++;
        if (!isdigit((unsigned char) *v->ptr)) {
            v->ok = false;
            return;
        }
        while (isdigit((unsigned char) *v->ptr))
            v->ptr++;
    }
    if (*v->ptr == 'e' || *v->ptr == 'E') {
        v->ptr++;
        if (*v->ptr == '+' || *v->ptr == '-')
            v->ptr++;
        if (!isdigit((unsigned char) *v->ptr)) {
            v->ok = false;
            return;
        }
        while (isdigit((unsigned char) *v->ptr))
            v->ptr++;
    }
}

static void validate_array(JsonValidator *v) {
    v->ptr++;
    validate_skip_ws(v);
    if (*v->ptr == ']') {
        v->ptr++;
        return;
    }
    while (v->ok) {
        validate_skip_ws(v);
        validate_value(v);
        if (!v->ok)
            return;
        validate_skip_ws(v);
        if (*v->ptr == ']') {
            v->ptr++;
            return;
        }
        if (*v->ptr != ',') {
            v->ok = false;
            return;
        }
        v->ptr++;
    }
}

static void validate_object(JsonValidator *v) {
    v->ptr++;
    validate_skip_ws(v);
    if (*v->ptr == '}') {
        v->ptr++;
        return;
    }
    while (v->ok) {
        validate_skip_ws(v);
        validate_string(v);
        if (!v->ok)
            return;
        validate_skip_ws(v);
        if (*v->ptr != ':') {
            v->ok = false;
            return;
        }
        v->ptr++;
        validate_skip_ws(v);
        validate_value(v);
        if (!v->ok)
            return;
        validate_skip_ws(v);
        if (*v->ptr == '}') {
            v->ptr++;
            return;
        }
        if (*v->ptr != ',') {
            v->ok = false;
            return;
        }
        v->ptr++;
    }
}

static void validate_value(JsonValidator *v) {
    if (!v->ok)
        return;
    if (v->depth >= JSON_MAX_DEPTH) {
        v->ok = false;
        return;
    }
    v->depth++;
    validate_skip_ws(v);
    switch (*v->ptr) {
        case '"':
            validate_string(v);
            break;
        case '[':
            validate_array(v);
            break;
        case '{':
            validate_object(v);
            break;
        case 't':
            if (strncmp(v->ptr, "true", 4) == 0) {
                v->ptr += 4;
            } else {
                v->ok = false;
            }
            break;
        case 'f':
            if (strncmp(v->ptr, "false", 5) == 0) {
                v->ptr += 5;
            } else {
                v->ok = false;
            }
            break;
        case 'n':
            if (strncmp(v->ptr, "null", 4) == 0) {
                v->ptr += 4;
            } else {
                v->ok = false;
            }
            break;
        default:
            if (*v->ptr == '-' || isdigit((unsigned char) *v->ptr)) {
                validate_number(v);
            } else {
                v->ok = false;
            }
            break;
    }
    v->depth--;
}

// isValid(str, strict?) - Check if string is valid JSON (zero allocation).
// strict (bool, default false): when true, additionally reject
// unescaped control bytes (< 0x20) inside strings, matching RFC 8259 §7.
XrValue xr_json_fn_is_valid(XrVMRuntime *X, XrValue self, XrValue *args, int argc) {
    (void) X;
    (void) self;
    if (argc < 1 || !XR_IS_STRING(args[0])) {
        return xr_bool(false);
    }

    bool strict = false;
    if (argc >= 2 && XR_IS_BOOL(args[1])) {
        strict = XR_TO_BOOL(args[1]);
    } else if (argc >= 2 && xr_value_is_json(args[1])) {
        // Legacy: accept {strict: true} options object
        XrJson *opts = xr_value_to_json(args[1]);
        XrValue sv = xr_json_get_by_key(X, opts, "strict");
        if (XR_IS_BOOL(sv))
            strict = XR_TO_BOOL(sv);
    }

    XrString *str = XR_TO_STRING(args[0]);
    JsonValidator v = {.ptr = str->data, .depth = 0, .ok = true, .strict = strict};

    validate_value(&v);
    if (!v.ok)
        return xr_bool(false);

    validate_skip_ws(&v);
    if (*v.ptr != '\0')
        return xr_bool(false);

    return xr_bool(true);
}

// tryParse(str) - Try to parse JSON
// Returns Json: {value: parsed result, error: error message or null}
XrValue xr_json_fn_try_parse(XrVMRuntime *X, XrValue self, XrValue *args, int argc) {
    (void) self;
    XrJson *result = xr_json_new(xr_current_coro(X));

    if (argc < 1 || !XR_IS_STRING(args[0])) {
        xr_json_set_by_key(
            X, result, "error",
            xr_string_value(xr_string_intern(X, "Argument must be a string", 25, 0)));
        xr_json_set_by_key(X, result, "value", xr_null());
        return xr_json_value(result);
    }

    XrString *str = XR_TO_STRING(args[0]);
    XrJsonValue *dom = xjson_parse(str->data, str->length);

    if (!dom) {
        const char *msg = "Invalid JSON";
        xr_json_set_by_key(X, result, "value", xr_null());
        xr_json_set_by_key(X, result, "error",
                           xr_string_value(xr_string_intern(X, msg, strlen(msg), 0)));
    } else {
        xr_json_set_by_key(X, result, "value", dom_to_xrvalue(X, dom));
        xr_json_set_by_key(X, result, "error", xr_null());
        xjson_free(dom);
    }

    return xr_json_value(result);
}
