/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xregex_binding.c - Xray regex module binding
 *
 * KEY CONCEPT:
 *   Bind C regex library to Xray runtime, providing script-level interface.
 */

#include "xregex_binding.h"
#include "xregex.h"
#include "../common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Xray runtime headers
#include "../../src/runtime/value/xvalue.h"
#include "../../src/runtime/value/xvalue_format.h"
#include "../../src/runtime/object/xstring.h"
#include "../../src/runtime/object/xarray.h"
#include "../../src/runtime/object/xjson.h"
#include "../../src/runtime/symbol/xsymbol_table.h"
#include "../../src/runtime/xisolate_internal.h"
#include "../../src/runtime/xisolate_api.h"
#include "../../src/coro/xcoroutine.h"
#include "../../src/runtime/gc/xcoro_gc.h"

/* ========================================================================
 * Helper Functions
 * ======================================================================== */

// Get string from XrValue
static const char *value_to_cstring(XrValue v, int *len) {
    if (!XR_IS_STRING(v))
        return NULL;
    XrString *s = XR_TO_STRING(v);
    if (!s)
        return NULL;
    if (len)
        *len = (int) s->length;
    return s->data;
}

/*
 * Parse flags string
 * "i" = ignore case
 * "m" = multiline mode
 * "s" = dot matches newline
 */
static XrRegexFlags parse_flags(const char *flags_str) {
    XrRegexFlags flags = XR_RE_NONE;
    if (!flags_str)
        return flags;

    for (const char *p = flags_str; *p; p++) {
        switch (*p) {
            case 'i':
                flags |= XR_RE_IGNORECASE;
                break;
            case 'm':
                flags |= XR_RE_MULTILINE;
                break;
            case 's':
                flags |= XR_RE_DOTALL;
                break;
        }
    }
    return flags;
}

/*
 * Create Match object (as Json)
 *
 * Structure:
 *   { start: int, end: int, text: string, groups: Array<string> }
 *
 * Public symbol: stdlib/regex/regex_methods.c reuses this so the new
 * XrMethodSlot wrappers and the legacy native-type binding both yield
 * identically shaped match objects.
 */
XrValue xr_regex_make_match_object(XrayIsolate *isolate, const char *text, XrMatch *match) {
    /* Temporarily disable GC: multiple allocations below (Json, String,
     * Array) are not rooted from the VM stack — only held in C locals.
     * A GC step triggered by any intermediate alloc could collect them. */
    XrCoroutine *coro = xr_current_coro(isolate);
    XrCoroGC *gc = coro ? coro->coro_gc : NULL;
    if (gc) gc->gc_disabled++;

    XrJson *result = xr_json_new(coro, 4);

    // start
    int start_offset = match->groups[0].start ? (int) (match->groups[0].start - text) : 0;
    xr_json_set_by_key(isolate, result, "start", xr_int(start_offset));

    // end
    int end_offset = match->groups[0].end ? (int) (match->groups[0].end - text) : 0;
    xr_json_set_by_key(isolate, result, "end", xr_int(end_offset));

    // text
    if (match->groups[0].start && match->groups[0].end) {
        int len = (int) (match->groups[0].end - match->groups[0].start);
        XrString *matched_text = xr_string_intern(isolate, match->groups[0].start, len, 0);
        xr_json_set_by_key(isolate, result, "text", xr_string_value(matched_text));
    } else {
        xr_json_set_by_key(isolate, result, "text", xr_null());
    }

    // groups
    XrArray *groups = xr_array_new(xr_current_coro(isolate));
    for (int i = 0; i < match->group_count; i++) {
        if (match->groups[i].start && match->groups[i].end) {
            int len = (int) (match->groups[i].end - match->groups[i].start);
            XrString *group_text = xr_string_intern(isolate, match->groups[i].start, len, 0);
            xr_array_push(groups, xr_string_value(group_text));
        } else {
            xr_array_push(groups, xr_null());
        }
    }
    xr_json_set_by_key(isolate, result, "groups", xr_value_from_array(groups));

    if (gc) gc->gc_disabled--;
    return xr_json_value(result);
}

/* ========================================================================
 * Regex Object Wrapper (using GC-managed XrRegexObject)
 * ======================================================================== */

#include "../../src/runtime/gc/xgc.h"

/*
 * XrRegexObject - GC-managed regex object
 *
 * Contains GC header and pointer to underlying XrRegex
 */
typedef struct XrRegexObject {
    XrGCHeader gc;   // GC header
    XrRegex *regex;  // underlying regex (compiled program)
} XrRegexObject;

// Type check macros
#define XR_IS_REGEX_OBJ(v)                                                                         \
    (XR_IS_PTR(v) && XR_GC_GET_TYPE((XrGCHeader *) XR_TO_PTR(v)) == XR_TREGEX)

#define XR_TO_REGEX_OBJ(v) ((XrRegexObject *) XR_TO_PTR(v))

// GC destructor for regex objects
void regex_object_destroy(XrGCHeader *obj, struct XrCoroGC *owning_gc) {
    (void) owning_gc;
    XrRegexObject *regex_obj = (XrRegexObject *) obj;
    if (regex_obj->regex) {
        xr_regex_free(regex_obj->regex);
        regex_obj->regex = NULL;
    }
}

// Create GC-managed regex object
static XrRegexObject *regex_object_new(XrayIsolate *isolate, XrRegex *re) {
    XrRegexObject *obj =
        (XrRegexObject *) xr_gc_alloc(&isolate->gc, sizeof(XrRegexObject), XR_TREGEX);
    obj->regex = re;
    return obj;
}

// Wrap XrRegex as XrValue (GC-managed)
static XrValue wrap_regex(XrayIsolate *isolate, XrRegex *re) {
    XrRegexObject *obj = regex_object_new(isolate, re);
    return XR_FROM_PTR(obj);
}

// Get XrRegex pointer from XrValue
static XrRegex *unwrap_regex(XrayIsolate *isolate, XrValue v) {
    (void) isolate;
    if (!XR_IS_REGEX_OBJ(v))
        return NULL;
    return XR_TO_REGEX_OBJ(v)->regex;
}

// Public API: wrap XrRegex as XrValue
XrValue xr_regex_wrap(XrayIsolate *isolate, XrRegex *re) {
    return wrap_regex(isolate, re);
}

// Public API: compile a regex literal (OP_REGEX_COMPILE bytecode helper).
// Both arguments must be strings; flag chars 'i' / 'm' / 's' map to
// XR_RE_IGNORECASE / MULTILINE / DOTALL, anything else is silently
// ignored to preserve the inline-parser behavior the VM dispatch had
// before the bridge existed.
XrValue xr_regex_compile_literal(XrayIsolate *isolate, XrValue pattern_val, XrValue flags_val) {
    XrString *pattern_str = xr_value_to_string(isolate, pattern_val);
    XrString *flags_str = xr_value_to_string(isolate, flags_val);
    if (!pattern_str || !flags_str) {
        return xr_null();
    }

    XrRegexFlags regex_flags = XR_RE_NONE;
    for (const char *p = flags_str->data; *p; p++) {
        switch (*p) {
            case 'i':
                regex_flags |= XR_RE_IGNORECASE;
                break;
            case 'm':
                regex_flags |= XR_RE_MULTILINE;
                break;
            case 's':
                regex_flags |= XR_RE_DOTALL;
                break;
            default:
                break; /* unknown flag chars are silently ignored */
        }
    }

    XrRegexError error;
    XrRegex *re = xr_regex_compile(pattern_str->data, regex_flags, &error);
    return re ? wrap_regex(isolate, re) : xr_null();
}

// Public API: check if value is Regex object
bool xr_value_is_regex(XrValue v) {
    return XR_IS_REGEX_OBJ(v);
}

// Public API: get Regex pointer
XrRegex *xr_value_to_regex(XrValue v) {
    if (!XR_IS_REGEX_OBJ(v))
        return NULL;
    return XR_TO_REGEX_OBJ(v)->regex;
}

/* ========================================================================
 * Exported Functions
 * ======================================================================== */

// compile(pattern [, flags]) - Compile regex
static XrValue regex_compile(XrayIsolate *isolate, XrValue *args, int argc) {
    if (argc < 1)
        return xr_null();

    int pattern_len;
    const char *pattern = value_to_cstring(args[0], &pattern_len);
    if (!pattern)
        return xr_null();

    XrRegexFlags flags = XR_RE_NONE;
    if (argc >= 2) {
        const char *flags_str = value_to_cstring(args[1], NULL);
        flags = parse_flags(flags_str);
    }

    XrRegexError error;
    XrRegex *re = xr_regex_compile(pattern, flags, &error);
    if (!re) {
        xr_runtime_error(isolate, "regex.compile: %s in pattern '%s'", xr_regex_error_str(error),
                         pattern);
        return xr_null();
    }

    return wrap_regex(isolate, re);
}

// test(re, text) - Test if matches
static XrValue regex_test(XrayIsolate *isolate, XrValue *args, int argc) {
    if (argc < 2)
        return xr_bool(false);

    XrRegex *re = unwrap_regex(isolate, args[0]);
    if (!re)
        return xr_bool(false);

    int text_len;
    const char *text = value_to_cstring(args[1], &text_len);
    if (!text)
        return xr_bool(false);

    bool result = xr_regex_test(re, text, text_len);
    return xr_bool(result);
}

// fullMatch(re, text) - Full match
static XrValue regex_full_match(XrayIsolate *isolate, XrValue *args, int argc) {
    if (argc < 2)
        return xr_null();

    XrRegex *re = unwrap_regex(isolate, args[0]);
    if (!re)
        return xr_null();

    int text_len;
    const char *text = value_to_cstring(args[1], &text_len);
    if (!text)
        return xr_null();

    XrMatch match;
    bool found = xr_regex_full_match(re, text, text_len, &match);
    if (!found)
        return xr_null();

    return xr_regex_make_match_object(isolate, text, &match);
}

// count(re, text) - Count matches
static XrValue regex_count(XrayIsolate *isolate, XrValue *args, int argc) {
    if (argc < 2)
        return xr_int(0);

    XrRegex *re = unwrap_regex(isolate, args[0]);
    if (!re)
        return xr_int(0);

    int text_len;
    const char *text = value_to_cstring(args[1], &text_len);
    if (!text)
        return xr_int(0);

    int count = xr_regex_count(re, text, text_len);
    return xr_int(count);
}

// find(re, text [, offset]) - Find match from specified position
static XrValue regex_find(XrayIsolate *isolate, XrValue *args, int argc) {
    if (argc < 2)
        return xr_null();

    XrRegex *re = unwrap_regex(isolate, args[0]);
    if (!re)
        return xr_null();

    int text_len;
    const char *text = value_to_cstring(args[1], &text_len);
    if (!text)
        return xr_null();

    int offset = 0;
    if (argc >= 3 && XR_IS_INT(args[2])) {
        offset = (int) XR_TO_INT(args[2]);
    }

    XrMatch match;
    bool found = xr_regex_match_at(re, text, text_len, offset, &match);
    if (!found)
        return xr_null();

    return xr_regex_make_match_object(isolate, text, &match);
}

// findAll(re, text [, limit]) - Find all matches
static XrValue regex_find_all(XrayIsolate *isolate, XrValue *args, int argc) {
    if (argc < 2)
        return xr_value_from_array(xr_array_new(xr_current_coro(isolate)));

    XrRegex *re = unwrap_regex(isolate, args[0]);
    if (!re)
        return xr_value_from_array(xr_array_new(xr_current_coro(isolate)));

    int text_len;
    const char *text = value_to_cstring(args[1], &text_len);
    if (!text)
        return xr_value_from_array(xr_array_new(xr_current_coro(isolate)));

    int limit = -1;
    if (argc >= 3 && XR_IS_INT(args[2])) {
        limit = (int) XR_TO_INT(args[2]);
    }

    int count = 0;
    XrMatch *matches = xr_regex_find_all(re, text, text_len, limit, &count);

    XrArray *result = xr_array_new(xr_current_coro(isolate));
    if (matches) {
        for (int i = 0; i < count; i++) {
            XrValue match_obj = xr_regex_make_match_object(isolate, text, &matches[i]);
            xr_array_push(result, match_obj);
        }
        xr_regex_find_all_free(matches);
    }

    return xr_value_from_array(result);
}

// replace(re, text, replacement) - Replace first match
static XrValue regex_replace(XrayIsolate *isolate, XrValue *args, int argc) {
    if (argc < 3)
        return xr_null();

    XrRegex *re = unwrap_regex(isolate, args[0]);
    if (!re)
        return xr_null();

    int text_len;
    const char *text = value_to_cstring(args[1], &text_len);
    if (!text)
        return xr_null();

    const char *repl = value_to_cstring(args[2], NULL);
    if (!repl)
        return xr_null();

    // Use dynamically allocated version
    char *result = xr_regex_replace_alloc(re, text, text_len, repl, false);
    if (!result)
        return args[1];  // no match, return original text

    XrString *result_str = xr_string_intern(isolate, result, strlen(result), 0);
    xr_free(result);

    return xr_string_value(result_str);
}

// replaceAll(re, text, replacement) - Replace all matches
static XrValue regex_replace_all(XrayIsolate *isolate, XrValue *args, int argc) {
    if (argc < 3)
        return xr_null();

    XrRegex *re = unwrap_regex(isolate, args[0]);
    if (!re)
        return xr_null();

    int text_len;
    const char *text = value_to_cstring(args[1], &text_len);
    if (!text)
        return xr_null();

    const char *repl = value_to_cstring(args[2], NULL);
    if (!repl)
        return xr_null();

    // Use dynamically allocated version
    char *result = xr_regex_replace_alloc(re, text, text_len, repl, true);
    if (!result)
        return args[1];

    XrString *result_str = xr_string_intern(isolate, result, strlen(result), 0);
    xr_free(result);

    return xr_string_value(result_str);
}

// split(re, text) - Split by pattern
static XrValue regex_split(XrayIsolate *isolate, XrValue *args, int argc) {
    if (argc < 2)
        return xr_value_from_array(xr_array_new(xr_current_coro(isolate)));

    XrRegex *re = unwrap_regex(isolate, args[0]);
    if (!re)
        return xr_value_from_array(xr_array_new(xr_current_coro(isolate)));

    int text_len;
    const char *text = value_to_cstring(args[1], &text_len);
    if (!text)
        return xr_value_from_array(xr_array_new(xr_current_coro(isolate)));

    int limit = -1;
    if (argc >= 3 && XR_IS_INT(args[2])) {
        limit = (int) XR_TO_INT(args[2]);
    }

    // Dynamic allocation to avoid stack overflow on large inputs
    int max_parts = (limit > 0 && limit < 256) ? limit : 256;
    XrSplitPart *parts = (XrSplitPart *) xr_malloc(max_parts * sizeof(XrSplitPart));
    int count = xr_regex_split(re, text, text_len, parts, max_parts, limit);

    XrArray *result = xr_array_new(xr_current_coro(isolate));
    for (int i = 0; i < count; i++) {
        XrString *part = xr_string_intern(isolate, parts[i].str, parts[i].len, 0);
        xr_array_push(result, xr_string_value(part));
    }

    xr_free(parts);
    return xr_value_from_array(result);
}

// escape(text) - Escape special characters
static XrValue regex_escape(XrayIsolate *isolate, XrValue *args, int argc) {
    if (argc < 1)
        return xr_null();

    int text_len;
    const char *text = value_to_cstring(args[0], &text_len);
    if (!text)
        return xr_null();

    // Worst case: every character needs escaping
    size_t buf_size = (size_t) (text_len * 2 + 1);
    char *escaped = (char *) xr_malloc(buf_size);
    if (!escaped)
        return args[0];

    int result_len = xr_regex_escape(text, text_len, escaped, buf_size);
    if (result_len < 0) {
        xr_free(escaped);
        return args[0];
    }

    XrString *result = xr_string_intern(isolate, escaped, result_len, 0);
    xr_free(escaped);

    return xr_string_value(result);
}

// isValid(pattern) - Check if pattern is valid
static XrValue regex_is_valid(XrayIsolate *isolate, XrValue *args, int argc) {
    if (argc < 1)
        return xr_bool(false);

    const char *pattern = value_to_cstring(args[0], NULL);
    if (!pattern)
        return xr_bool(false);

    (void) isolate;
    return xr_bool(xr_regex_is_valid(pattern, XR_RE_NONE));
}

/* ========================================================================
 * Regex Object Methods (native type methods)
 * Support re.test(text) syntax
 * ======================================================================== */

#include "../../src/runtime/object/xnative_type.h"

// re.pattern getter
static XrValue re_method_pattern(XrayIsolate *isolate, XrValue self, XrValue *args, int nargs) {
    (void) args;
    (void) nargs;

    XrRegex *re = unwrap_regex(isolate, self);
    if (!re)
        return xr_null();

    const char *pattern = xr_regex_pattern(re);
    if (!pattern)
        return xr_null();

    return xr_string_value(xr_string_intern(isolate, pattern, strlen(pattern), 0));
}

// Thin wrappers: prepend self into a temporary args array so the module
// functions (which expect args[0]=regex) can be reused unchanged.

static XrValue re_m_test(XrayIsolate *X, XrValue self, XrValue *a, int n) {
    XrValue tmp[3] = {self, n > 0 ? a[0] : xr_null(), n > 1 ? a[1] : xr_null()};
    return regex_test(X, tmp, n + 1);
}
static XrValue re_m_find(XrayIsolate *X, XrValue self, XrValue *a, int n) {
    XrValue tmp[4] = {self, n > 0 ? a[0] : xr_null(), n > 1 ? a[1] : xr_null()};
    return regex_find(X, tmp, n + 1);
}
static XrValue re_m_find_all(XrayIsolate *X, XrValue self, XrValue *a, int n) {
    XrValue tmp[4] = {self, n > 0 ? a[0] : xr_null(), n > 1 ? a[1] : xr_null()};
    return regex_find_all(X, tmp, n + 1);
}
static XrValue re_m_replace(XrayIsolate *X, XrValue self, XrValue *a, int n) {
    XrValue tmp[4] = {self, n > 0 ? a[0] : xr_null(), n > 1 ? a[1] : xr_null()};
    return regex_replace(X, tmp, n + 1);
}
static XrValue re_m_replace_all(XrayIsolate *X, XrValue self, XrValue *a, int n) {
    XrValue tmp[4] = {self, n > 0 ? a[0] : xr_null(), n > 1 ? a[1] : xr_null()};
    return regex_replace_all(X, tmp, n + 1);
}
static XrValue re_m_split(XrayIsolate *X, XrValue self, XrValue *a, int n) {
    XrValue tmp[4] = {self, n > 0 ? a[0] : xr_null(), n > 1 ? a[1] : xr_null()};
    return regex_split(X, tmp, n + 1);
}

static XrNativeMethod regex_methods[] = {
    {"test", re_m_test, 1},
    {"find", re_m_find, 2},
    {"findAll", re_m_find_all, 2},
    {"replace", re_m_replace, 2},
    {"replaceAll", re_m_replace_all, 2},
    {"split", re_m_split, 2},
    {NULL, NULL, 0}};

// Regex property getters
static XrNativeMethod regex_getters[] = {
    {"pattern", re_method_pattern, 1},
    {NULL, NULL, 0}};

/* ========================================================================
 * Native Type Registration
 * ========================================================================
 *
 * Registers the Regex XrClass so regex literals (`/foo/`) and
 * `regex.compile(...)` results dispatch object methods through
 * native_type_classes[XR_TREGEX]. Invoked unconditionally during
 * isolate init by xr_prelude_register_all_native_types.
 *
 * xr_register_native_type is idempotent (returns the existing class on
 * a second call for the same gc_type), so older xisolate_full.c / load
 * paths that explicitly poke this function still work — they just turn
 * into no-ops once prelude has already registered the class.
 */

void xr_regex_register_native_type(XrayIsolate *isolate) {
    static const XrNativeTypeInfo regex_info = {
        .name = "regex",
        .gc_type = XR_TREGEX,
        .methods = regex_methods,
        .getters = regex_getters,
        .static_methods = NULL,
    };
    xr_register_native_type(isolate, &regex_info);
}

/* ========================================================================
 * Module Loading
 * ======================================================================== */

// ========== Type Declarations (parsed by gen_stdlib_types.py) ==========

#include "../../src/module/xbuiltin_decl.h"

// @module regex

XR_DEFINE_BUILTIN(regex_compile, "compile", "(pattern: string, flags?: string): Regex",
                  "Compile a regex pattern")
XR_DEFINE_BUILTIN(regex_test, "test", "(pattern: Regex, s: string): bool",
                  "Test if pattern matches string")
XR_DEFINE_BUILTIN(regex_find, "find", "(pattern: Regex, s: string, offset?: int): Json",
                  "Find first match")
XR_DEFINE_BUILTIN(regex_full_match, "fullFind", "(pattern: Regex, s: string): Json",
                  "Full match with captures")
XR_DEFINE_BUILTIN(regex_count, "count", "(pattern: Regex, s: string): int", "Count matches")
XR_DEFINE_BUILTIN(regex_find_all, "findAll", "(pattern: Regex, s: string): Array<string>",
                  "Find all matches")
XR_DEFINE_BUILTIN(regex_replace, "replace",
                  "(pattern: Regex, s: string, replacement: string): string",
                  "Replace first match")
XR_DEFINE_BUILTIN(regex_replace_all, "replaceAll",
                  "(pattern: Regex, s: string, replacement: string): string",
                  "Replace all matches")
XR_DEFINE_BUILTIN(regex_split, "split", "(pattern: Regex, s: string): Array<string>",
                  "Split by pattern")
XR_DEFINE_BUILTIN(regex_escape, "escape", "(s: string): string", "Escape regex special chars")
XR_DEFINE_BUILTIN(regex_is_valid, "isValid", "(pattern: string): bool", "Check if pattern is valid")

XR_FUNC XrModule *xr_load_module_regex(XrayIsolate *isolate) {
    // 1. Create native module
    XrModule *mod = xr_module_create_native(isolate, "regex");
    if (!mod)
        return NULL;

    XRS_EXPORT(mod, isolate, "compile", regex_compile);
    XRS_EXPORT(mod, isolate, "test", regex_test);
    XRS_EXPORT(mod, isolate, "find", regex_find);
    XRS_EXPORT(mod, isolate, "fullFind", regex_full_match);
    XRS_EXPORT(mod, isolate, "count", regex_count);
    XRS_EXPORT(mod, isolate, "findAll", regex_find_all);
    XRS_EXPORT(mod, isolate, "replace", regex_replace);
    XRS_EXPORT(mod, isolate, "replaceAll", regex_replace_all);
    XRS_EXPORT(mod, isolate, "split", regex_split);
    XRS_EXPORT(mod, isolate, "escape", regex_escape);
    XRS_EXPORT(mod, isolate, "isValid", regex_is_valid);

    // The Regex XrClass itself is registered up front by the prelude
    // module — no need to do it again here.
    mod->loaded = true;
    return mod;
}
