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
#include "../../src/runtime/class/xinstance.h"
#include "../../src/runtime/symbol/xsymbol_table.h"
#include "../../src/runtime/class/xclass.h"
#include "../../src/runtime/class/xclass_builder.h"
#include "../../src/runtime/class/xclass_system.h"
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
 * Public symbol: stdlib/regex/regex_methods.c reuses this so the
 * native-type binding yields identically shaped match objects.
 */
XrValue xr_regex_make_match_object(XrayIsolate *isolate, const char *text, XrMatch *match) {
    /* Temporarily disable GC: multiple allocations below (Json, String,
     * Array) are not rooted from the VM stack — only held in C locals.
     * A GC step triggered by any intermediate alloc could collect them. */
    XrCoroutine *coro = xr_current_coro(isolate);
    XrCoroGC *gc = coro ? coro->coro_gc : NULL;
    if (gc)
        gc->gc_disabled++;

    XrJson *result = xr_json_new(coro);

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

    if (gc)
        gc->gc_disabled--;
    return xr_json_value(result);
}

/* ========================================================================
 * Regex Object Wrapper (XrInstance + native body)
 *
 * The native body stores a single XrRegex* pointer.  The class is
 * registered as regexClass with builtin_kind == XR_BK_REGEX so type
 * checks and formatting use a single field test, no dedicated GC tag needed.
 * ======================================================================== */

/* Native body layout: stored after XrInstance fields[] */
typedef struct {
    XrRegex *regex;
} RegexBody;

/* Retrieve native body from an XrInstance known to be a Regex */
static inline RegexBody *regex_body(XrInstance *inst) {
    return (RegexBody *) xr_instance_native_body(inst);
}

/* Check whether v is a Regex instance (builtin_kind test) */
static inline bool is_regex_instance(XrValue v) {
    if (!XR_IS_INSTANCE(v))
        return false;
    XrInstance *inst = (XrInstance *) XR_TO_PTR(v);
    return inst->klass && inst->klass->builtin_kind == XR_BK_REGEX;
}

/* Create a Regex XrInstance wrapping a compiled XrRegex */
static XrValue wrap_regex(XrayIsolate *isolate, XrRegex *re) {
    XrayCoreClasses *core = xr_isolate_get_core_classes(isolate);
    XR_DCHECK(core && core->regexClass, "wrap_regex: regexClass not registered");
    XrCoroutine *coro = xr_current_coro(isolate);
    XrInstance *inst = xr_instance_new(coro, core->regexClass);
    if (!inst)
        return xr_null();
    RegexBody *body = regex_body(inst);
    body->regex = re;
    return XR_FROM_PTR(inst);
}

/* Unwrap XrRegex* from an XrValue */
static XrRegex *unwrap_regex(XrayIsolate *isolate, XrValue v) {
    (void) isolate;
    if (!is_regex_instance(v))
        return NULL;
    XrInstance *inst = (XrInstance *) XR_TO_PTR(v);
    return regex_body(inst)->regex;
}

/* Public API: wrap XrRegex as XrValue */
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

/* Public API: check if value is Regex object */
bool xr_value_is_regex(XrValue v) {
    return is_regex_instance(v);
}

/* Public API: get Regex pointer from value */
XrRegex *xr_value_to_regex(XrValue v) {
    if (!is_regex_instance(v))
        return NULL;
    XrInstance *inst = (XrInstance *) XR_TO_PTR(v);
    return regex_body(inst)->regex;
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

/* ========================================================================
 * Native Body Lifecycle
 * ======================================================================== */

static void regex_body_destroy(void *body) {
    RegexBody *rb = (RegexBody *) body;
    if (rb->regex) {
        xr_regex_free(rb->regex);
        rb->regex = NULL;
    }
}

static XrNativeBodyDesc g_regex_body_desc = {
    .body_size = sizeof(RegexBody),
    .body_align = _Alignof(void *),
    .copy_policy = XR_NATIVE_BODY_COPY_FORBID,
    .init = NULL,
    .destroy = regex_body_destroy,
    .traverse = NULL,
    .deep_copy = NULL,
    .to_shared = NULL,
};

/* ========================================================================
 * Class Registration
 *
 * Builds the Regex XrClass with native body descriptor and installs
 * instance methods (test, find, etc.) so OP_INVOKE resolves them
 * through the unified XrClass dispatch.
 * ======================================================================== */

static XrValue re_m_to_string(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    return xr_string_value(xr_value_to_string(iso, self));
}

void xr_regex_register_class(XrayIsolate *isolate) {
    XR_DCHECK(isolate != NULL, "regex_register_class: NULL isolate");
    XrayCoreClasses *core = xr_isolate_get_core_classes(isolate);
    XR_DCHECK(core != NULL, "regex_register_class: core not initialised");
    XR_DCHECK(core->regexClass == NULL, "regex_register_class: already registered");

    XrClassBuilder *b = xr_class_builder_new(isolate, "Regex", core->objectClass);
    XR_CHECK(b != NULL, "regex_register_class: builder alloc failed");

    xr_class_builder_set_native_body(b, &g_regex_body_desc);

    /* Instance methods */
    xr_class_builder_add_method(b, "test", re_m_test, 0, 1);
    xr_class_builder_add_method(b, "find", re_m_find, 0, 2);
    xr_class_builder_add_method(b, "findAll", re_m_find_all, 0, 2);
    xr_class_builder_add_method(b, "replace", re_m_replace, 0, 2);
    xr_class_builder_add_method(b, "replaceAll", re_m_replace_all, 0, 2);
    xr_class_builder_add_method(b, "split", re_m_split, 0, 2);
    xr_class_builder_add_method(b, "pattern", re_method_pattern, 0, 0);
    xr_class_builder_add_method(b, "toString", re_m_to_string, 0, 0);

    XrClass *cls = xr_class_builder_finalize(b);
    XR_CHECK(cls != NULL, "regex_register_class: finalize failed");
    cls->flags |= XR_CLASS_BUILTIN | XR_CLASS_HAS_NATIVE_BODY;
    cls->builtin_kind = XR_BK_REGEX;
    core->regexClass = cls;
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
                  "(pattern: Regex, s: string, replacement: string): string", "Replace first match")
XR_DEFINE_BUILTIN(regex_replace_all, "replaceAll",
                  "(pattern: Regex, s: string, replacement: string): string", "Replace all matches")
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
