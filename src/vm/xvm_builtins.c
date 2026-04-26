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
 * stdlib/datetime/datetime.h is no longer included on this side of
 * the architecture boundary. */
#include "../../stdlib/regex/xregex.h"
#include "../../stdlib/regex/xregex_binding.h"
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
