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
#include "../../stdlib/datetime/datetime.h"
#include "../../stdlib/regex/xregex.h"
#include "../../stdlib/regex/xregex_binding.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* ========== Unified Method Call Dispatch (Jump Table Optimized) ========== */

/* === Map Method Handlers === */

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

// Map method dispatch - switch optimized to jump table by compiler
XrValue map_method_call_by_symbol(XrayIsolate *isolate, XrMap *map, int symbol, XrValue *args, int argc) {
    XR_DCHECK(isolate != NULL, "map_dispatch: NULL isolate");
    XR_DCHECK(map != NULL, "map_dispatch: NULL map");
    XR_DCHECK(XR_GC_GET_TYPE(&map->gc) == XR_TMAP, "map_dispatch: object is not a map");
    XrValue receiver = xr_value_from_map(map);

    bool is_weak = (map->flags & XR_MAP_FLAG_WEAK) != 0;

    // Method dispatch
    if (symbol == SYMBOL_IS_EMPTY) return map_is_empty_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_HAS) return map_has_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_GET) return map_get_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_DELETE) return map_delete_handler(isolate, receiver, args, argc);

    if (symbol == SYMBOL_SET) {
        // WeakMap: key must be a GC object (not int/float/string/bool/null).
        // Contract violation — surface it as a catchable exception so
        // user code can recover via try/catch instead of leaving a
        // stderr breadcrumb and silently returning null.
        if (is_weak && argc >= 1 && !XR_VALUE_NEEDS_GC(args[0])) {
            XrValue exc = xr_exception_newf(isolate, XR_ERR_INVALID_ARG_TYPE,
                "WeakMap key must be a heap object, got %s",
                xr_typeid_name(xr_value_typeid(args[0])));
            xr_vm_unwind_with_trace(isolate, exc);
            return xr_null();
        }
        return map_set_handler(isolate, receiver, args, argc);
    }

    // WeakMap: block enumeration methods
    if (is_weak) {
        if (symbol == SYMBOL_KEYS || symbol == SYMBOL_VALUES ||
            symbol == SYMBOL_ENTRIES || symbol == SYMBOL_CLEAR ||
            symbol == SYMBOL_HAS_VALUE_MAP || symbol == SYMBOL_ITERATOR ||
            symbol == SYMBOL_ENTRIES_ITERATOR) {
            return XR_NOTFOUND;
        }
    }

    if (symbol == SYMBOL_HAS_VALUE_MAP) return map_has_value_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_CLEAR) return map_clear_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_KEYS) return map_keys_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_VALUES) return map_values_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_ENTRIES) return map_entries_handler(isolate, receiver, args, argc);

    // Iterator method
    if (symbol == SYMBOL_ITERATOR) {
        return xr_null();
    }

    // EntriesIterator method - for (key, value in map) support
    if (symbol == SYMBOL_ENTRIES_ITERATOR) {
        XrIterator *iter = xr_map_entries_iterator(isolate,  map);
        return iter ? xr_value_from_iterator(iter) : xr_null();
    }

    // toString fallback
    if (symbol == SYMBOL_TOSTRING) {
        XrValue map_val = XR_FROM_PTR(map);
        return xr_string_value(xr_value_to_string(isolate, map_val));
    }

    // Method not found — caller (OP_INVOKE_BUILTIN) throws catchable error
    return XR_NOTFOUND;
}

// Json method dispatch
XrValue json_method_call_by_symbol(XrayIsolate *isolate, XrJson *json, int symbol, XrValue *args, int argc) {
    XR_DCHECK(isolate != NULL, "json_dispatch: NULL isolate");
    (void)args; (void)argc;

    // Internal protocol: for (key, value in obj) support
    if (symbol == SYMBOL_ENTRIES_ITERATOR) {
        XrCoroutine *coro = xr_current_coro(isolate);
        XrIterator *iter = xr_iterator_new_from_json(coro, json, isolate);
        return iter ? xr_value_from_iterator(iter) : xr_null();
    }

    // toString fallback
    if (symbol == SYMBOL_TOSTRING) {
        XrValue json_val = xr_json_value(json);
        return xr_string_value(xr_value_to_string(isolate, json_val));
    }

    // All other methods removed: use Json.xxx(obj) static methods instead
    return XR_NOTFOUND;
}

/* === String Method Handlers === */

static XrValue string_charat_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)isolate;
    if (argc < 1) return xr_null();
    XrString *str = XR_TO_STRING(receiver);
    xr_Integer index = XR_TO_INT(args[0]);

    // Handle negative index: -1 means last character
    if (index < 0) {
        size_t char_len = xr_string_char_length(str);
        index = (xr_Integer)char_len + index;
        if (index < 0) return xr_null();  // Out of bounds
    }

    // Use Unicode character index
    XrString *result = xr_string_char_at_unicode(isolate, str, (size_t)index);
    return result ? xr_string_value(result) : xr_null();
}

// codePointAt(index) - return Unicode codepoint at position
static XrValue string_codepoint_at_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)isolate;
    if (argc < 1) return xr_int(-1);
    XrString *str = XR_TO_STRING(receiver);
    size_t index = (size_t)XR_TO_INT(args[0]);
    int32_t cp = xr_string_char_code_at(str, index);
    return xr_int(cp);
}

static XrValue string_substring_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)isolate;
    XrString *str = XR_TO_STRING(receiver);
    if (argc < 1) return xr_string_value(str);
    xr_Integer start = XR_TO_INT(args[0]);
    xr_Integer end = (argc >= 2) ? XR_TO_INT(args[1]) : -1;
    XrString *result = xr_string_substring(isolate, str, start, end);
    return result ? xr_string_value(result) : xr_null();
}

// slice(start, end) - slice with negative index support
static XrValue string_slice_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    XrString *str = XR_TO_STRING(receiver);
    if (argc < 1) return xr_string_value(str);
    xr_Integer start = XR_TO_INT(args[0]);
    // If only one argument, end defaults to string length
    xr_Integer end = (argc >= 2) ? XR_TO_INT(args[1]) : (xr_Integer)str->length;
    XrString *result = xr_string_slice(isolate, str, start, end);
    return result ? xr_string_value(result) : xr_null();
}

static XrValue string_indexof_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)isolate;
    if (argc < 1 || !XR_IS_STRING(args[0])) return xr_int(-1);
    XrString *str = XR_TO_STRING(receiver);
    XrString *substr = xr_value_to_string(isolate, args[0]);
    return xr_int(xr_string_index_of(isolate, str, substr));
}

static XrValue string_contains_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)isolate;
    if (argc < 1 || !XR_IS_STRING(args[0])) return xr_bool(0);
    XrString *str = XR_TO_STRING(receiver);
    XrString *substr = xr_value_to_string(isolate, args[0]);
    return xr_bool(xr_string_has(isolate, str, substr));
}

static XrValue string_startswith_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)isolate;
    if (argc < 1 || !XR_IS_STRING(args[0])) return xr_bool(0);
    XrString *str = XR_TO_STRING(receiver);
    XrString *prefix = xr_value_to_string(isolate, args[0]);
    return xr_bool(xr_string_starts_with(isolate, str, prefix));
}

static XrValue string_endswith_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)isolate;
    if (argc < 1 || !XR_IS_STRING(args[0])) return xr_bool(0);
    XrString *str = XR_TO_STRING(receiver);
    XrString *suffix = xr_value_to_string(isolate, args[0]);
    return xr_bool(xr_string_ends_with(isolate, str, suffix));
}

static XrValue string_tolowercase_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)args; (void)argc;
    XrString *str = XR_TO_STRING(receiver);
    XrString *result = xr_string_to_lower_case(isolate, str);
    return result ? xr_string_value(result) : xr_string_value(str);
}

static XrValue string_touppercase_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)args; (void)argc;
    XrString *str = XR_TO_STRING(receiver);
    XrString *result = xr_string_to_upper_case(isolate, str);
    return result ? xr_string_value(result) : xr_string_value(str);
}

static XrValue string_trim_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)args; (void)argc;
    XrString *str = XR_TO_STRING(receiver);
    XrString *result = xr_string_trim(isolate, str);
    return result ? xr_string_value(result) : xr_string_value(str);
}

static XrValue string_trim_start_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)args; (void)argc;
    XrString *str = XR_TO_STRING(receiver);
    XrString *result = xr_string_trim_start(isolate, str);
    return result ? xr_string_value(result) : xr_string_value(str);
}

static XrValue string_trim_end_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)args; (void)argc;
    XrString *str = XR_TO_STRING(receiver);
    XrString *result = xr_string_trim_end(isolate, str);
    return result ? xr_string_value(result) : xr_string_value(str);
}

static XrValue string_pad_start_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    XrString *str = XR_TO_STRING(receiver);
    if (argc < 1) return xr_string_value(str);
    size_t target_len = (size_t)XR_TO_INT(args[0]);
    XrString *pad_str = (argc >= 2 && XR_IS_STRING(args[1])) ? xr_value_to_string(isolate, args[1]) : NULL;
    XrString *result = xr_string_pad_start(isolate, str, target_len, pad_str);
    return result ? xr_string_value(result) : xr_string_value(str);
}

static XrValue string_pad_end_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    XrString *str = XR_TO_STRING(receiver);
    if (argc < 1) return xr_string_value(str);
    size_t target_len = (size_t)XR_TO_INT(args[0]);
    XrString *pad_str = (argc >= 2 && XR_IS_STRING(args[1])) ? xr_value_to_string(isolate, args[1]) : NULL;
    XrString *result = xr_string_pad_end(isolate, str, target_len, pad_str);
    return result ? xr_string_value(result) : xr_string_value(str);
}

static XrValue string_last_index_of_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    XrString *str = XR_TO_STRING(receiver);
    if (argc < 1 || !XR_IS_STRING(args[0])) return xr_int(-1);
    XrString *substr = xr_value_to_string(isolate, args[0]);
    xr_Integer result = xr_string_last_index_of(isolate, str, substr);
    return xr_int(result);
}

static XrValue string_split_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    XrString *str = XR_TO_STRING(receiver);
    if (argc < 1) {
        XrArray *arr = xr_array_new(xr_current_coro(isolate));
        xr_array_push(arr, xr_string_value(str));
        return xr_value_from_array(arr);
    }
    if (!XR_IS_STRING(args[0])) return xr_null();
    XrString *delimiter = xr_value_to_string(isolate, args[0]);
    XrArray *result = xr_string_split(isolate, str, delimiter);
    return result ? xr_value_from_array(result) : xr_null();
}

static XrValue string_replace_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)isolate;
    XrString *str = XR_TO_STRING(receiver);
    if (argc < 2 || !XR_IS_STRING(args[0]) || !XR_IS_STRING(args[1])) {
        return xr_string_value(str);
    }
    XrString *old_str = xr_value_to_string(isolate, args[0]);
    XrString *new_str = xr_value_to_string(isolate, args[1]);
    XrString *result = xr_string_replace(isolate, str, old_str, new_str);
    return result ? xr_string_value(result) : xr_string_value(str);
}

static XrValue string_replaceall_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)isolate;
    XrString *str = XR_TO_STRING(receiver);
    if (argc < 2 || !XR_IS_STRING(args[0]) || !XR_IS_STRING(args[1])) {
        return xr_string_value(str);
    }
    XrString *old_str = xr_value_to_string(isolate, args[0]);
    XrString *new_str = xr_value_to_string(isolate, args[1]);
    XrString *result = xr_string_replace_all(isolate, str, old_str, new_str);
    return result ? xr_string_value(result) : xr_string_value(str);
}

static XrValue string_repeat_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    XrString *str = XR_TO_STRING(receiver);
    if (argc < 1) return xr_string_value(str);
    xr_Integer count = XR_TO_INT(args[0]);
    if (count <= 0) return xr_string_value(xr_string_intern(isolate, "", 0, 0));
    XrString *result = xr_string_repeat(isolate, str, count);
    return result ? xr_string_value(result) : xr_null();
}

static XrValue string_concat_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)isolate;
    XrString *str = XR_TO_STRING(receiver);
    if (argc < 1) return xr_string_value(str);

    // Concatenate all arguments
    XrString *result = str;
    for (int i = 0; i < argc; i++) {
        if (XR_IS_STRING(args[i])) {
            XrString *other = xr_value_to_string(isolate, args[i]);
            result = xr_string_concat(isolate, result, other);
            if (!result) return xr_string_value(str);
        }
    }
    return xr_string_value(result);
}

static XrValue string_is_empty_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)args; (void)argc;
    XrString *str = XR_TO_STRING(receiver);
    return xr_bool(xr_string_is_empty(isolate, str));
}

// reverse() - reverse string (Unicode safe)
static XrValue string_reverse_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)args; (void)argc;
    XrString *str = XR_TO_STRING(receiver);
    XrString *result = xr_string_reverse(isolate, str);
    return result ? xr_string_value(result) : xr_null();
}

// reverseBytes() - byte-level reverse (O(n), for ASCII/DNA)
static XrValue string_reverse_bytes_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)args; (void)argc;
    XrString *str = XR_TO_STRING(receiver);
    XrString *result = xr_string_reverse_bytes(isolate, str);
    return result ? xr_string_value(result) : xr_null();
}

// byteAt(i) - O(1) byte index (supports negative index)
static XrValue string_byte_at_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    if (argc < 1) return xr_null();
    XrString *str = XR_TO_STRING(receiver);
    xr_Integer index = XR_TO_INT(args[0]);
    XrString *result = xr_string_byte_at(isolate, str, index);
    return result ? xr_string_value(result) : xr_null();
}

// translateBytes(table) - byte-level character mapping
static XrValue string_translate_bytes_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_MAP(args[0])) return receiver;  // No mapping table, return original
    XrString *str = XR_TO_STRING(receiver);
    XrMap *table = XR_TO_MAP(args[0]);
    XrString *result = xr_string_translate_bytes(isolate, str, table);
    return result ? xr_string_value(result) : xr_null();
}

// translate(table) - Unicode character mapping
static XrValue string_translate_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_MAP(args[0])) return receiver;
    XrString *str = XR_TO_STRING(receiver);
    XrMap *table = XR_TO_MAP(args[0]);
    XrString *result = xr_string_translate(isolate, str, table);
    return result ? xr_string_value(result) : xr_null();
}

static XrValue string_has_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0])) return xr_bool(0);
    XrString *str = XR_TO_STRING(receiver);
    XrString *substr = xr_value_to_string(isolate, args[0]);
    return xr_bool(xr_string_has(isolate, str, substr));
}

// toInt() - string to integer
static XrValue string_toint_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)isolate; (void)args; (void)argc;
    XrString *str = XR_TO_STRING(receiver);

    // Skip leading whitespace
    const char *p = str->data;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;

    // Parse integer
    char *end;
    long long value = strtoll(p, &end, 10);

    // Check if valid number
    if (end == p) {
        return xr_null();  // Parse failed, return null
    }

    return xr_int((xr_Integer)value);
}

// toFloat() - string to float
static XrValue string_tofloat_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)isolate; (void)args; (void)argc;
    XrString *str = XR_TO_STRING(receiver);

    // Skip leading whitespace
    const char *p = str->data;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;

    // Parse float
    char *end;
    double value = strtod(p, &end);

    // Check if valid number
    if (end == p) {
        return xr_null();  // Parse failed, return null
    }

    return xr_float(value);
}

// isLetter() - check if string is all letters
static XrValue string_is_letter_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)isolate; (void)args; (void)argc;
    XrString *str = XR_TO_STRING(receiver);
    return xr_bool(xr_string_is_letter(str));
}

// isNumber() - check if string is all digits
static XrValue string_is_number_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)isolate; (void)args; (void)argc;
    XrString *str = XR_TO_STRING(receiver);
    return xr_bool(xr_string_is_number(str));
}

// isAlphanumeric() - check if string is all letters or digits
static XrValue string_is_alnum_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)isolate; (void)args; (void)argc;
    XrString *str = XR_TO_STRING(receiver);
    return xr_bool(xr_string_is_alnum(str));
}

// isWhitespace() - check if string is all whitespace
static XrValue string_is_whitespace_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)isolate; (void)args; (void)argc;
    XrString *str = XR_TO_STRING(receiver);
    return xr_bool(xr_string_is_whitespace_str(str));
}

// ord() - return Unicode codepoint of first character
static XrValue string_ord_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)isolate; (void)args; (void)argc;
    XrString *str = XR_TO_STRING(receiver);
    int32_t cp = xr_string_ord(str);
    return cp >= 0 ? xr_int(cp) : xr_null();
}

// match(regex) - regex match, return match result array or null
static XrValue string_match_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    if (argc < 1) return xr_null();

    XrString *str = XR_TO_STRING(receiver);

    // Check if argument is regex
    if (!xr_value_is_regex(args[0])) {
        return xr_null();
    }

    XrRegex *re = xr_value_to_regex(args[0]);
    if (!re) return xr_null();

    // Execute match
    XrMatch match;
    bool found = xr_regex_match(re, str->data, (int)str->length, &match);

    if (!found) return xr_null();

    // Return match result array
    XrArray *result = xr_array_new(xr_current_coro(isolate));
    for (int i = 0; i < match.group_count; i++) {
        if (match.groups[i].start) {
            size_t len = match.groups[i].end - match.groups[i].start;
            XrString *group_str = xr_string_intern(isolate, match.groups[i].start, len, 0);
            xr_array_push(result, xr_string_value(group_str));
        } else {
            xr_array_push(result, xr_null());
        }
    }

    return xr_value_from_array(result);
}

// String method dispatch - switch optimized to jump table by compiler
XrValue string_method_call_by_symbol(XrayIsolate *isolate, XrString *str, int symbol, XrValue *args, int argc) {
    XR_DCHECK(isolate != NULL, "string_dispatch: NULL isolate");
    XR_DCHECK(str != NULL, "string_dispatch: NULL string");
    XR_DCHECK(XR_GC_GET_TYPE(&str->gc) == XR_TSTRING, "string_dispatch: object is not a string");
    // Keep heap form to avoid SSO→promote round-trip in handlers
    XrValue receiver = XR_FROM_PTR(str);

    // Method dispatch

    if (symbol == SYMBOL_IS_EMPTY) return string_is_empty_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_HAS) return string_has_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_CHARAT) return string_charat_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_SUBSTRING) return string_substring_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_SLICE) return string_slice_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_INDEXOF) return string_indexof_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_CONTAINS) return string_contains_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_STARTSWITH) return string_startswith_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_ENDSWITH) return string_endswith_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_TOLOWERCASE) return string_tolowercase_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_TOUPPERCASE) return string_touppercase_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_TRIM) return string_trim_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_TRIM_START) return string_trim_start_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_TRIM_END) return string_trim_end_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_PAD_START) return string_pad_start_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_PAD_END) return string_pad_end_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_LASTINDEXOF) return string_last_index_of_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_SPLIT) return string_split_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_REPLACE) return string_replace_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_REPLACEALL) return string_replaceall_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_REPEAT) return string_repeat_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_CONCAT) return string_concat_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_CODEPOINT_AT || symbol == SYMBOL_CHARCODEAT) return string_codepoint_at_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_REVERSE) return string_reverse_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_REVERSE_BYTES) return string_reverse_bytes_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_BYTE_AT) return string_byte_at_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_TRANSLATE) return string_translate_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_TRANSLATE_BYTES) return string_translate_bytes_handler(isolate, receiver, args, argc);

    // Type conversion methods
    if (symbol == SYMBOL_TOINT) return string_toint_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_TOFLOAT) return string_tofloat_handler(isolate, receiver, args, argc);

    // Character classification methods
    if (symbol == SYMBOL_IS_LETTER) return string_is_letter_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_IS_NUMBER) return string_is_number_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_IS_ALNUM) return string_is_alnum_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_IS_WHITESPACE) return string_is_whitespace_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_ORD) return string_ord_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_MATCH) return string_match_handler(isolate, receiver, args, argc);

    // toString - return self
    if (symbol == SYMBOL_TOSTRING) return xr_string_value(str);

    // Method not found — caller (OP_INVOKE_BUILTIN) throws catchable error
    return XR_NOTFOUND;
}

/* === Array Method Handlers === */

static XrValue array_pop_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)isolate; (void)args; (void)argc;
    XrArray *array = XR_TO_ARRAY(receiver);
    if (array->length > 0) {
        return xr_array_pop(array);
    }
    return xr_null();
}

static XrValue array_join_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    XrArray *array = XR_TO_ARRAY(receiver);
    if (argc < 1 || !XR_IS_STRING(args[0])) {
        return xr_string_value(xr_string_intern(isolate, "", 0, 0));
    }
    XrString *delimiter = xr_value_to_string(isolate, args[0]);
    XrString *result = xr_array_join(isolate, array, delimiter);
    return result ? xr_string_value(result) : xr_null();
}

static XrValue array_reverse_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)isolate; (void)args; (void)argc;
    XrArray *array = XR_TO_ARRAY(receiver);
    xr_array_reverse(array);
    return xr_value_from_array(array);
}

static XrValue array_shift_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)isolate; (void)args; (void)argc;
    XrArray *array = XR_TO_ARRAY(receiver);
    return xr_array_shift(array);
}

static XrValue array_unshift_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)isolate;
    if (argc < 1) return xr_int(0);
    XrArray *array = XR_TO_ARRAY(receiver);
    xr_array_unshift(array, args[0]);
    return xr_int((xr_Integer)xr_array_size(array));
}

static XrValue array_indexof_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)isolate;
    if (argc < 1) return xr_int(-1);
    XrArray *array = XR_TO_ARRAY(receiver);
    int index = xr_array_index_of(array, args[0]);
    return xr_int((xr_Integer)index);
}

static XrValue array_clear_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)isolate; (void)args; (void)argc;
    XrArray *array = XR_TO_ARRAY(receiver);
    xr_array_clear(array);
    return xr_null();
}

static XrValue array_is_empty_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)isolate; (void)args; (void)argc;
    XrArray *array = XR_TO_ARRAY(receiver);
    return xr_bool(xr_array_is_empty(array));
}

static XrValue array_has_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)isolate;
    if (argc < 1) return xr_bool(0);
    XrArray *array = XR_TO_ARRAY(receiver);
    return xr_bool(xr_array_has(array, args[0]));
}

// Higher-order function handlers
static XrValue array_foreach_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    if (argc < 1) {
        return xr_null();
    }
    XrArray *array = XR_TO_ARRAY(receiver);
    struct XrClosure *callback = (struct XrClosure*)XR_TO_PTR(args[0]);
    xr_array_foreach(isolate, array, callback);
    return xr_null();
}

static XrValue array_filter_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    if (argc < 1) {
        return xr_value_from_array(xr_array_new(xr_current_coro(isolate)));
    }
    XrArray *array = XR_TO_ARRAY(receiver);
    struct XrClosure *callback = (struct XrClosure*)XR_TO_PTR(args[0]);
    XrArray *result = xr_array_filter(isolate, array, callback);
    return xr_value_from_array(result);
}

static XrValue array_map_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    if (argc < 1) {
        return xr_value_from_array(xr_array_new(xr_current_coro(isolate)));
    }
    XrArray *array = XR_TO_ARRAY(receiver);
    struct XrClosure *callback = (struct XrClosure*)XR_TO_PTR(args[0]);
    XrArray *result = xr_array_map(isolate, array, callback);
    return xr_value_from_array(result);
}

static XrValue array_reduce_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    if (argc < 2) {
        return xr_null();
    }
    XrArray *array = XR_TO_ARRAY(receiver);
    struct XrClosure *callback = (struct XrClosure*)XR_TO_PTR(args[0]);
    XrValue initial = args[1];
    return xr_array_reduce(isolate, array, callback, initial);
}

// Array method dispatch - switch optimized to jump table by compiler
XrValue array_method_call_by_symbol(XrayIsolate *isolate, XrArray *array, int symbol, XrValue *args, int argc) {
    XR_DCHECK(isolate != NULL, "array_dispatch: NULL isolate");
    XR_DCHECK(array != NULL, "array_dispatch: NULL array");
    XR_DCHECK(XR_GC_GET_TYPE(&array->gc) == XR_TARRAY || XR_GC_GET_TYPE(&array->gc) == XR_TARRAY_SLICE,
              "array_dispatch: object is not an array");
    XrValue receiver = xr_value_from_array(array);

    // Method dispatch
    if (symbol == SYMBOL_IS_EMPTY) return array_is_empty_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_HAS) return array_has_handler(isolate, receiver, args, argc);

    // Inline push optimization - eliminate function call overhead
    if (symbol == SYMBOL_PUSH) {
        if (argc >= 1) {
            // Inline capacity check and write
            if (array->length >= array->capacity) {
                xr_array_grow(array);
                // Defensive: if grow failed silently, fall back to safe path
                if (array->length >= array->capacity) {
                    xr_array_push(array, args[0]);
                    return receiver;
                }
            }
            if (array->elem_type == XR_ELEM_ANY) {
                ((XrValue*)array->data)[array->length++] = args[0];
                // Write barrier: notify incremental GC that black container was mutated
                XR_GC_BARRIER_BACK_SAFE(xr_current_coro_gc(), array);
            } else {
                xr_array_set_element(array, array->length++, args[0]);
            }
        }
        // Return this for chaining: arr.push(1).push(2)
        return receiver;
    }

    if (symbol == SYMBOL_POP) return array_pop_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_SHIFT) return array_shift_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_UNSHIFT) return array_unshift_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_INDEXOF) return array_indexof_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_JOIN) return array_join_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_REVERSE) return array_reverse_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_CLEAR) return array_clear_handler(isolate, receiver, args, argc);

    // slice(start, end?) - return new sub-array (bulk memcpy)
    if (symbol == SYMBOL_SLICE) {
        int len = (int)array->length;
        int start = 0, end = len;
        if (argc >= 1 && XR_IS_INT(args[0])) {
            start = (int)XR_TO_INT(args[0]);
            if (start < 0) start = len + start;
            if (start < 0) start = 0;
        }
        if (argc >= 2 && XR_IS_INT(args[1])) {
            end = (int)XR_TO_INT(args[1]);
            if (end < 0) end = len + end;
            if (end > len) end = len;
        }
        if (start >= end || start >= len) {
            return xr_value_from_array(xr_array_new(xr_current_coro(isolate)));
        }
        int count = end - start;
        XrArray *result;
        if (array->elem_type != XR_ELEM_ANY) {
            result = xr_array_with_capacity_typed(xr_current_coro(isolate), count, (XrArrayElemType)array->elem_type);
            if (result) {
                result->elem_tid = array->elem_tid;
                memcpy(result->data, (uint8_t*)array->data + (size_t)start * array->elem_size,
                       (size_t)count * array->elem_size);
                result->length = count;
            }
        } else {
            result = xr_array_with_capacity(xr_current_coro(isolate), count);
            if (result) {
                memcpy(result->data, (XrValue*)array->data + start, (size_t)count * sizeof(XrValue));
                result->length = count;
                result->has_gc_ptrs = array->has_gc_ptrs;
            }
        }
        return xr_value_from_array(result ? result : xr_array_new(xr_current_coro(isolate)));
    }

    // concat(other) - return new merged array (bulk memcpy)
    if (symbol == SYMBOL_CONCAT) {
        // Calculate total capacity
        int total = (int)array->length;
        for (int i = 0; i < argc; i++) {
            if (XR_IS_ARRAY(args[i]))
                total += (int)XR_TO_ARRAY(args[i])->length;
            else
                total += 1;
        }
        XrArray *result = xr_array_with_capacity(xr_current_coro(isolate), total);
        if (!result) return xr_value_from_array(xr_array_new(xr_current_coro(isolate)));
        // Copy source array
        if (array->length > 0) {
            memcpy(result->data, array->data, (size_t)array->length * array->elem_size);
            result->length = array->length;
            result->has_gc_ptrs = array->has_gc_ptrs;
        }
        // Append each argument
        for (int i = 0; i < argc; i++) {
            if (XR_IS_ARRAY(args[i])) {
                XrArray *other = XR_TO_ARRAY(args[i]);
                if (other->length > 0) {
                    xr_array_ensure_capacity(result, result->length + other->length);
                    memcpy((XrValue*)result->data + result->length, other->data,
                           (size_t)other->length * sizeof(XrValue));
                    result->length += other->length;
                    if (other->has_gc_ptrs) result->has_gc_ptrs = 1;
                }
            } else {
                xr_array_push(result, args[i]);
            }
        }
        return xr_value_from_array(result);
    }

    // Higher-order functions - runtime calls
    if (symbol == SYMBOL_FOREACH) return array_foreach_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_FILTER) return array_filter_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_MAP_METHOD) return array_map_handler(isolate, receiver, args, argc);
    if (symbol == SYMBOL_REDUCE) return array_reduce_handler(isolate, receiver, args, argc);

    if (symbol == SYMBOL_FIND) {
        if (argc < 1) return xr_null();
        struct XrClosure *cb = (struct XrClosure*)XR_TO_PTR(args[0]);
        return xr_array_find(isolate, array, cb);
    }
    if (symbol == SYMBOL_FINDINDEX) {
        if (argc < 1) return xr_int(-1);
        struct XrClosure *cb = (struct XrClosure*)XR_TO_PTR(args[0]);
        return xr_int(xr_array_find_index(isolate, array, cb));
    }
    if (symbol == SYMBOL_EVERY) {
        if (argc < 1) return xr_bool(true);
        struct XrClosure *cb = (struct XrClosure*)XR_TO_PTR(args[0]);
        return xr_bool(xr_array_every(isolate, array, cb));
    }
    if (symbol == SYMBOL_SOME) {
        if (argc < 1) return xr_bool(false);
        struct XrClosure *cb = (struct XrClosure*)XR_TO_PTR(args[0]);
        return xr_bool(xr_array_some(isolate, array, cb));
    }
    if (symbol == SYMBOL_FILL) {
        if (argc < 1) return receiver;
        XrValue fill_val = args[0];
        int start = 0, end = (int)array->length;
        if (argc >= 2 && XR_IS_INT(args[1])) start = (int)XR_TO_INT(args[1]);
        if (argc >= 3 && XR_IS_INT(args[2])) end = (int)XR_TO_INT(args[2]);
        xr_array_fill(array, fill_val, start, end);
        return receiver;
    }
    if (symbol == SYMBOL_SORT) {
        struct XrClosure *cmp = NULL;
        if (argc >= 1 && XR_IS_PTR(args[0])) {
            cmp = (struct XrClosure*)XR_TO_PTR(args[0]);
        }
        xr_array_sort(isolate, array, cmp);
        return receiver;
    }

    // includes(value) - check if array contains value, return bool
    if (symbol == SYMBOL_INCLUDES) {
        if (argc < 1) return xr_bool(false);
        return xr_bool(xr_array_has(array, args[0]));
    }

    // toString fallback
    if (symbol == SYMBOL_TOSTRING) {
        return xr_string_value(xr_value_to_string(isolate, receiver));
    }

    // Method not found — caller (OP_INVOKE_BUILTIN) throws catchable error
    return XR_NOTFOUND;
}

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

/* ========== DateTime Method Dispatch ========== */

// DateTime method dispatch
XrValue datetime_method_call_by_symbol(XrayIsolate *isolate, void *dt, int symbol, XrValue *args, int argc) {
    XR_DCHECK(isolate != NULL, "datetime_dispatch: NULL isolate");
    XrDateTime *datetime = (XrDateTime*)dt;
    (void)args; (void)argc;

    // format(pattern) - format datetime
    if (symbol == SYMBOL_FORMAT) {
        const char *pattern = XR_DATETIME_DEFAULT_FORMAT;
        if (argc > 0 && XR_IS_STRING(args[0])) {
            pattern = xr_value_to_string(isolate, args[0])->data;
        }
        char buf[256];
        int len = xr_datetime_format(datetime, pattern, buf, sizeof(buf));
        return xr_string_value(xr_string_intern(isolate, buf, (size_t)len, 0));
    }

    // year() - get year
    if (symbol == SYMBOL_YEAR) {
        return xr_int(xr_datetime_year(datetime));
    }

    // month() - get month
    if (symbol == SYMBOL_MONTH) {
        return xr_int(xr_datetime_month(datetime));
    }

    // day() - get day
    if (symbol == SYMBOL_DAY) {
        return xr_int(xr_datetime_day(datetime));
    }

    // hour() - get hour
    if (symbol == SYMBOL_HOUR) {
        return xr_int(xr_datetime_hour(datetime));
    }

    // minute() - get minute
    if (symbol == SYMBOL_MINUTE) {
        return xr_int(xr_datetime_minute(datetime));
    }

    // second() - get second
    if (symbol == SYMBOL_SECOND) {
        return xr_int(xr_datetime_second(datetime));
    }

    // weekday() - get weekday
    if (symbol == SYMBOL_WEEKDAY) {
        return xr_int(xr_datetime_weekday(datetime));
    }

    // timestamp() - get timestamp
    if (symbol == SYMBOL_TIMESTAMP) {
        return xr_int(datetime->timestamp);
    }

    // toUTC() - convert to UTC
    if (symbol == SYMBOL_TO_UTC) {
        XrDateTime *utc_dt = xr_datetime_to_utc(isolate, datetime);
        return utc_dt ? xr_datetime_value(utc_dt) : xr_null();
    }

    // toLocal() - convert to local time
    if (symbol == SYMBOL_TO_LOCAL) {
        XrDateTime *local_dt = xr_datetime_to_local(isolate, datetime);
        return local_dt ? xr_datetime_value(local_dt) : xr_null();
    }

    // millisecond() - get millisecond
    if (symbol == SYMBOL_MILLISECOND) {
        return xr_int(xr_datetime_millisecond(datetime));
    }

    // yearday() - get day of year
    if (symbol == SYMBOL_YEARDAY) {
        return xr_int(xr_datetime_yearday(datetime));
    }

    // isBefore(other) - compare
    if (symbol == SYMBOL_IS_BEFORE) {
        if (argc < 1 || !XR_IS_DATETIME(args[0])) return XR_FALSE_VAL;
        return xr_datetime_is_before(datetime, XR_TO_DATETIME(args[0])) ? XR_TRUE_VAL : XR_FALSE_VAL;
    }

    // isAfter(other) - compare
    if (symbol == SYMBOL_IS_AFTER) {
        if (argc < 1 || !XR_IS_DATETIME(args[0])) return XR_FALSE_VAL;
        return xr_datetime_is_after(datetime, XR_TO_DATETIME(args[0])) ? XR_TRUE_VAL : XR_FALSE_VAL;
    }

    // equals(other) - compare
    if (symbol == SYMBOL_EQUALS) {
        if (argc < 1 || !XR_IS_DATETIME(args[0])) return XR_FALSE_VAL;
        return xr_datetime_equals(datetime, XR_TO_DATETIME(args[0])) ? XR_TRUE_VAL : XR_FALSE_VAL;
    }

    // isLeapYear() - check leap year
    if (symbol == SYMBOL_IS_LEAP_YEAR) {
        return xr_datetime_is_leap_year(datetime) ? XR_TRUE_VAL : XR_FALSE_VAL;
    }

    // daysInMonth() - get days in month
    if (symbol == SYMBOL_DAYS_IN_MONTH) {
        return xr_int(xr_datetime_days_in_month(datetime));
    }

    // toISOString() - ISO 8601 format
    if (symbol == SYMBOL_TO_ISO_STRING) {
        char buf[64];
        int len = xr_datetime_to_iso_string(datetime, buf, sizeof(buf));
        return xr_string_value(xr_string_intern(isolate, buf, (size_t)len, 0));
    }

    // toString() - convert to string
    if (symbol == SYMBOL_TOSTRING) {
        char buf[256];
        int len = xr_datetime_format(datetime, XR_DATETIME_DEFAULT_FORMAT, buf, sizeof(buf));
        return xr_string_value(xr_string_intern(isolate, buf, (size_t)len, 0));
    }

    // Method not found — caller (OP_INVOKE_BUILTIN) throws catchable error
    return XR_NOTFOUND;
}

/* ========== BoundMethod Standalone Handlers ========== */

// Array push - extracted from inline code in dispatch
static XrValue array_push_handler(XrayIsolate *isolate, XrValue receiver, XrValue *args, int argc) {
    (void)isolate;
    XrArray *array = XR_TO_ARRAY(receiver);
    if (argc >= 1) {
        if (array->length >= array->capacity) {
            xr_array_grow(array);
        }
        if (array->elem_type == XR_ELEM_ANY) {
            ((XrValue*)array->data)[array->length++] = args[0];
        } else {
            xr_array_set_element(array, array->length++, args[0]);
        }
    }
    return receiver;
}

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
    if (symbol == SYMBOL_PUSH) return array_push_handler;
    if (symbol == SYMBOL_POP) return array_pop_handler;
    if (symbol == SYMBOL_SHIFT) return array_shift_handler;
    if (symbol == SYMBOL_UNSHIFT) return array_unshift_handler;
    if (symbol == SYMBOL_INDEXOF) return array_indexof_handler;
    if (symbol == SYMBOL_HAS) return array_has_handler;
    if (symbol == SYMBOL_JOIN) return array_join_handler;
    if (symbol == SYMBOL_REVERSE) return array_reverse_handler;
    if (symbol == SYMBOL_CLEAR) return array_clear_handler;
    if (symbol == SYMBOL_IS_EMPTY) return array_is_empty_handler;
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
    if (symbol == SYMBOL_HAS) return string_contains_handler;
    if (symbol == SYMBOL_CHARAT) return string_charat_handler;
    if (symbol == SYMBOL_SUBSTRING) return string_substring_handler;
    if (symbol == SYMBOL_INDEXOF) return string_indexof_handler;
    if (symbol == SYMBOL_CONTAINS) return string_contains_handler;
    if (symbol == SYMBOL_STARTSWITH) return string_startswith_handler;
    if (symbol == SYMBOL_ENDSWITH) return string_endswith_handler;
    if (symbol == SYMBOL_TOLOWERCASE) return string_tolowercase_handler;
    if (symbol == SYMBOL_TOUPPERCASE) return string_touppercase_handler;
    if (symbol == SYMBOL_TRIM) return string_trim_handler;
    if (symbol == SYMBOL_SPLIT) return string_split_handler;
    if (symbol == SYMBOL_REPLACE) return string_replace_handler;
    if (symbol == SYMBOL_REPLACEALL) return string_replaceall_handler;
    if (symbol == SYMBOL_REPEAT) return string_repeat_handler;
    if (symbol == SYMBOL_CONCAT) return string_concat_handler;
    if (symbol == SYMBOL_CODEPOINT_AT) return string_codepoint_at_handler;
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
