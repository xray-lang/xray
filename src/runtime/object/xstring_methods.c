/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstring_methods.c - String instance method bodies + dispatch table.
 *
 * The bodies here are thin adapters over xr_string_*() in xstring.c.
 * Each adapter:
 *   - validates the receiver via XR_DCHECK,
 *   - matches the legacy semantics (e.g. "return self on bad arg"),
 *   - returns XrValue with explicit null/bool/int/string boxing.
 *
 * The match() body bridges into stdlib/regex; see xstring_methods.h
 * for the architectural caveat.
 */

#include "xstring_methods.h"
#include "xstring.h"
#include "xarray.h"
#include "xmap.h"
#include "../value/xvalue.h"
#include "../value/xvalue_format.h"
#include "../symbol/xsymbol_table.h"
#include "../../coro/xcoroutine.h"
#include "../../base/xchecks.h"
#include "../../../stdlib/regex/xregex.h"
#include "../../../stdlib/regex/xregex_binding.h"
#include <stdlib.h>
#include <string.h>

static inline XrString *str_self(XrValue self) {
    XR_DCHECK(XR_IS_STRING(self), "string method: receiver is not a string");
    return XR_TO_STRING(self);
}

/* === Indexing / extraction === */

static XrValue m_char_at(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    if (argc < 1)
        return xr_null();
    XrString *str = str_self(self);
    xr_Integer index = XR_TO_INT(args[0]);
    if (index < 0) {
        size_t char_len = xr_string_char_length(str);
        index = (xr_Integer) char_len + index;
        if (index < 0)
            return xr_null();
    }
    XrString *result = xr_string_char_at_unicode(iso, str, (size_t) index);
    return result ? xr_string_value(result) : xr_null();
}

static XrValue m_codepoint_at(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    if (argc < 1)
        return xr_int(-1);
    XrString *str = str_self(self);
    size_t index = (size_t) XR_TO_INT(args[0]);
    return xr_int(xr_string_char_code_at(str, index));
}

static XrValue m_substring(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    XrString *str = str_self(self);
    if (argc < 1)
        return xr_string_value(str);
    xr_Integer start = XR_TO_INT(args[0]);
    xr_Integer end = (argc >= 2) ? XR_TO_INT(args[1]) : -1;
    XrString *result = xr_string_substring(iso, str, start, end);
    return result ? xr_string_value(result) : xr_null();
}

static XrValue m_slice(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    XrString *str = str_self(self);
    if (argc < 1)
        return xr_string_value(str);
    xr_Integer start = XR_TO_INT(args[0]);
    xr_Integer end = (argc >= 2) ? XR_TO_INT(args[1]) : (xr_Integer) str->length;
    XrString *result = xr_string_slice(iso, str, start, end);
    return result ? xr_string_value(result) : xr_null();
}

static XrValue m_byte_at(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    if (argc < 1)
        return xr_null();
    XrString *str = str_self(self);
    xr_Integer index = XR_TO_INT(args[0]);
    XrString *result = xr_string_byte_at(iso, str, index);
    return result ? xr_string_value(result) : xr_null();
}

/* === Search === */

static XrValue m_index_of(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0]))
        return xr_int(-1);
    XrString *str = str_self(self);
    XrString *substr = xr_value_to_string(iso, args[0]);
    return xr_int(xr_string_index_of(iso, str, substr));
}

static XrValue m_last_index_of(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    XrString *str = str_self(self);
    if (argc < 1 || !XR_IS_STRING(args[0]))
        return xr_int(-1);
    XrString *substr = xr_value_to_string(iso, args[0]);
    return xr_int(xr_string_last_index_of(iso, str, substr));
}

static XrValue m_contains(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0]))
        return xr_bool(0);
    XrString *str = str_self(self);
    XrString *substr = xr_value_to_string(iso, args[0]);
    return xr_bool(xr_string_has(iso, str, substr));
}

/* SYMBOL_HAS — same as contains but lives at a different symbol slot
 * for back-compat with code that uses the generic `has` name. */
static XrValue m_has(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    return m_contains(iso, self, args, argc);
}

static XrValue m_starts_with(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0]))
        return xr_bool(0);
    XrString *str = str_self(self);
    XrString *prefix = xr_value_to_string(iso, args[0]);
    return xr_bool(xr_string_starts_with(iso, str, prefix));
}

static XrValue m_ends_with(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0]))
        return xr_bool(0);
    XrString *str = str_self(self);
    XrString *suffix = xr_value_to_string(iso, args[0]);
    return xr_bool(xr_string_ends_with(iso, str, suffix));
}

/* === Case / whitespace transforms === */

static XrValue m_to_lower(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrString *str = str_self(self);
    XrString *result = xr_string_to_lower_case(iso, str);
    return result ? xr_string_value(result) : xr_string_value(str);
}

static XrValue m_to_upper(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrString *str = str_self(self);
    XrString *result = xr_string_to_upper_case(iso, str);
    return result ? xr_string_value(result) : xr_string_value(str);
}

static XrValue m_trim(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrString *str = str_self(self);
    XrString *result = xr_string_trim(iso, str);
    return result ? xr_string_value(result) : xr_string_value(str);
}

static XrValue m_trim_start(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrString *str = str_self(self);
    XrString *result = xr_string_trim_start(iso, str);
    return result ? xr_string_value(result) : xr_string_value(str);
}

static XrValue m_trim_end(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrString *str = str_self(self);
    XrString *result = xr_string_trim_end(iso, str);
    return result ? xr_string_value(result) : xr_string_value(str);
}

static XrValue m_pad_start(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    XrString *str = str_self(self);
    if (argc < 1)
        return xr_string_value(str);
    size_t target_len = (size_t) XR_TO_INT(args[0]);
    XrString *pad = (argc >= 2 && XR_IS_STRING(args[1])) ? xr_value_to_string(iso, args[1]) : NULL;
    XrString *result = xr_string_pad_start(iso, str, target_len, pad);
    return result ? xr_string_value(result) : xr_string_value(str);
}

static XrValue m_pad_end(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    XrString *str = str_self(self);
    if (argc < 1)
        return xr_string_value(str);
    size_t target_len = (size_t) XR_TO_INT(args[0]);
    XrString *pad = (argc >= 2 && XR_IS_STRING(args[1])) ? xr_value_to_string(iso, args[1]) : NULL;
    XrString *result = xr_string_pad_end(iso, str, target_len, pad);
    return result ? xr_string_value(result) : xr_string_value(str);
}

/* === Replacement / construction === */

static XrValue m_split(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    XrString *str = str_self(self);
    if (argc < 1) {
        XrArray *arr = xr_array_new(xr_current_coro(iso));
        xr_array_push(arr, xr_string_value(str));
        return xr_value_from_array(arr);
    }
    if (!XR_IS_STRING(args[0]))
        return xr_null();
    XrString *delim = xr_value_to_string(iso, args[0]);
    XrArray *result = xr_string_split(iso, str, delim);
    return result ? xr_value_from_array(result) : xr_null();
}

static XrValue m_replace(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    XrString *str = str_self(self);
    if (argc < 2 || !XR_IS_STRING(args[0]) || !XR_IS_STRING(args[1])) {
        return xr_string_value(str);
    }
    XrString *old_str = xr_value_to_string(iso, args[0]);
    XrString *new_str = xr_value_to_string(iso, args[1]);
    XrString *result = xr_string_replace(iso, str, old_str, new_str);
    return result ? xr_string_value(result) : xr_string_value(str);
}

static XrValue m_replace_all(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    XrString *str = str_self(self);
    if (argc < 2 || !XR_IS_STRING(args[0]) || !XR_IS_STRING(args[1])) {
        return xr_string_value(str);
    }
    XrString *old_str = xr_value_to_string(iso, args[0]);
    XrString *new_str = xr_value_to_string(iso, args[1]);
    XrString *result = xr_string_replace_all(iso, str, old_str, new_str);
    return result ? xr_string_value(result) : xr_string_value(str);
}

static XrValue m_repeat(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    XrString *str = str_self(self);
    if (argc < 1)
        return xr_string_value(str);
    xr_Integer count = XR_TO_INT(args[0]);
    if (count <= 0)
        return xr_string_value(xr_string_intern(iso, "", 0, 0));
    XrString *result = xr_string_repeat(iso, str, count);
    return result ? xr_string_value(result) : xr_null();
}

static XrValue m_concat(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    XrString *str = str_self(self);
    if (argc < 1)
        return xr_string_value(str);
    XrString *result = str;
    for (int i = 0; i < argc; i++) {
        if (XR_IS_STRING(args[i])) {
            XrString *other = xr_value_to_string(iso, args[i]);
            result = xr_string_concat(iso, result, other);
            if (!result)
                return xr_string_value(str);
        }
    }
    return xr_string_value(result);
}

/* === Reverse / translate === */

static XrValue m_reverse(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrString *str = str_self(self);
    XrString *result = xr_string_reverse(iso, str);
    return result ? xr_string_value(result) : xr_null();
}

static XrValue m_reverse_bytes(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrString *str = str_self(self);
    XrString *result = xr_string_reverse_bytes(iso, str);
    return result ? xr_string_value(result) : xr_null();
}

static XrValue m_translate_bytes(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_MAP(args[0]))
        return self;
    XrString *str = str_self(self);
    XrMap *table = XR_TO_MAP(args[0]);
    XrString *result = xr_string_translate_bytes(iso, str, table);
    return result ? xr_string_value(result) : xr_null();
}

static XrValue m_translate(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_MAP(args[0]))
        return self;
    XrString *str = str_self(self);
    XrMap *table = XR_TO_MAP(args[0]);
    XrString *result = xr_string_translate(iso, str, table);
    return result ? xr_string_value(result) : xr_null();
}

/* === Predicates / classification === */

static XrValue m_is_empty(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    return xr_bool(xr_string_is_empty(iso, str_self(self)));
}

static XrValue m_is_letter(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    return xr_bool(xr_string_is_letter(str_self(self)));
}

static XrValue m_is_number(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    return xr_bool(xr_string_is_number(str_self(self)));
}

static XrValue m_is_alnum(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    return xr_bool(xr_string_is_alnum(str_self(self)));
}

static XrValue m_is_whitespace(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    return xr_bool(xr_string_is_whitespace_str(str_self(self)));
}

/* === Conversion / Unicode === */

static XrValue m_to_int(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    XrString *str = str_self(self);
    const char *p = str->data;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    char *end;
    long long value = strtoll(p, &end, 10);
    if (end == p)
        return xr_null();
    return xr_int((xr_Integer) value);
}

static XrValue m_to_float(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    XrString *str = str_self(self);
    const char *p = str->data;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    char *end;
    double value = strtod(p, &end);
    if (end == p)
        return xr_null();
    return xr_float(value);
}

static XrValue m_ord(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    int32_t cp = xr_string_ord(str_self(self));
    return cp >= 0 ? xr_int(cp) : xr_null();
}

/* === Regex bridge === */

static XrValue m_match(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    if (argc < 1)
        return xr_null();
    XrString *str = str_self(self);
    if (!xr_value_is_regex(args[0]))
        return xr_null();
    XrRegex *re = xr_value_to_regex(args[0]);
    if (!re)
        return xr_null();

    XrMatch match;
    if (!xr_regex_match(re, str->data, (int) str->length, &match)) {
        return xr_null();
    }
    XrArray *result = xr_array_new(xr_current_coro(iso));
    for (int i = 0; i < match.group_count; i++) {
        if (match.groups[i].start) {
            size_t len = match.groups[i].end - match.groups[i].start;
            XrString *group = xr_string_intern(iso, match.groups[i].start, len, 0);
            xr_array_push(result, xr_string_value(group));
        } else {
            xr_array_push(result, xr_null());
        }
    }
    return xr_value_from_array(result);
}

/* === toString === */

static XrValue m_to_string(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    (void) args;
    (void) argc;
    return xr_string_value(str_self(self));
}

/* === Method table === */

#define PURE_NO_GC (XR_METHOD_FLAG_PURE | XR_METHOD_FLAG_NO_GC)
#define MAY_THROW XR_METHOD_FLAG_MAY_THROW

const XrMethodSlot xr_string_method_table[SYMBOL_BUILTIN_COUNT] = {
    /* Indexing / extraction */
    [SYMBOL_CHARAT] = {m_char_at, 1, 1, MAY_THROW},
    [SYMBOL_CODEPOINT_AT] = {m_codepoint_at, 1, 1, PURE_NO_GC},
    [SYMBOL_CHARCODEAT] = {m_codepoint_at, 1, 1, PURE_NO_GC},
    [SYMBOL_SUBSTRING] = {m_substring, 0, 2, MAY_THROW},
    [SYMBOL_SLICE] = {m_slice, 0, 2, MAY_THROW},
    [SYMBOL_BYTE_AT] = {m_byte_at, 1, 1, MAY_THROW},

    /* Search */
    [SYMBOL_INDEXOF] = {m_index_of, 1, 1, PURE_NO_GC},
    [SYMBOL_LASTINDEXOF] = {m_last_index_of, 1, 1, PURE_NO_GC},
    [SYMBOL_CONTAINS] = {m_contains, 1, 1, PURE_NO_GC},
    [SYMBOL_HAS] = {m_has, 1, 1, PURE_NO_GC},
    [SYMBOL_STARTSWITH] = {m_starts_with, 1, 1, PURE_NO_GC},
    [SYMBOL_ENDSWITH] = {m_ends_with, 1, 1, PURE_NO_GC},

    /* Case / whitespace */
    [SYMBOL_TOLOWERCASE] = {m_to_lower, 0, 0, MAY_THROW},
    [SYMBOL_TOUPPERCASE] = {m_to_upper, 0, 0, MAY_THROW},
    [SYMBOL_TRIM] = {m_trim, 0, 0, MAY_THROW},
    [SYMBOL_TRIM_START] = {m_trim_start, 0, 0, MAY_THROW},
    [SYMBOL_TRIM_END] = {m_trim_end, 0, 0, MAY_THROW},
    [SYMBOL_PAD_START] = {m_pad_start, 1, 2, MAY_THROW},
    [SYMBOL_PAD_END] = {m_pad_end, 1, 2, MAY_THROW},

    /* Replacement / construction */
    [SYMBOL_SPLIT] = {m_split, 0, 1, MAY_THROW},
    [SYMBOL_REPLACE] = {m_replace, 2, 2, MAY_THROW},
    [SYMBOL_REPLACEALL] = {m_replace_all, 2, 2, MAY_THROW},
    [SYMBOL_REPEAT] = {m_repeat, 1, 1, MAY_THROW},
    [SYMBOL_CONCAT] = {m_concat, 0, -1, MAY_THROW},

    /* Reverse / translate */
    [SYMBOL_REVERSE] = {m_reverse, 0, 0, MAY_THROW},
    [SYMBOL_REVERSE_BYTES] = {m_reverse_bytes, 0, 0, MAY_THROW},
    [SYMBOL_TRANSLATE_BYTES] = {m_translate_bytes, 1, 1, MAY_THROW},
    [SYMBOL_TRANSLATE] = {m_translate, 1, 1, MAY_THROW},

    /* Predicates */
    [SYMBOL_IS_EMPTY] = {m_is_empty, 0, 0, PURE_NO_GC},
    [SYMBOL_IS_LETTER] = {m_is_letter, 0, 0, PURE_NO_GC},
    [SYMBOL_IS_NUMBER] = {m_is_number, 0, 0, PURE_NO_GC},
    [SYMBOL_IS_ALNUM] = {m_is_alnum, 0, 0, PURE_NO_GC},
    [SYMBOL_IS_WHITESPACE] = {m_is_whitespace, 0, 0, PURE_NO_GC},

    /* Conversion / Unicode */
    [SYMBOL_TOINT] = {m_to_int, 0, 0, PURE_NO_GC},
    [SYMBOL_TOFLOAT] = {m_to_float, 0, 0, PURE_NO_GC},
    [SYMBOL_ORD] = {m_ord, 0, 0, PURE_NO_GC},

    /* Regex */
    [SYMBOL_MATCH] = {m_match, 1, 1, MAY_THROW},

    [SYMBOL_TOSTRING] = {m_to_string, 0, 0, PURE_NO_GC},
};

#undef PURE_NO_GC
#undef MAY_THROW
