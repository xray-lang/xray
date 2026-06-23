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
#include "../../src/runtime/class/xinstance.h"
#include "../../src/runtime/symbol/xsymbol_table.h"
#include "../../src/runtime/class/xclass.h"
#include "../../src/runtime/class/xclass_builder.h"
#include "../../src/runtime/class/xclass_system.h"
#include "../../src/shared/xr_regex_core.h"
#include "../../src/runtime/xisolate_internal.h"
#include "../../src/runtime/xisolate_api.h"
#include "../../src/coro/xcoroutine.h"
#include "../../src/runtime/mem/xcoro_heap.h"

_Static_assert((int) XR_RE_IGNORECASE == (int) XR_REGEX_CORE_FLAG_IGNORECASE,
               "regex ignorecase flag drift");
_Static_assert((int) XR_RE_MULTILINE == (int) XR_REGEX_CORE_FLAG_MULTILINE,
               "regex multiline flag drift");
_Static_assert((int) XR_RE_DOTALL == (int) XR_REGEX_CORE_FLAG_DOTALL, "regex dotall flag drift");

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
static XrRegexFlags parse_flags(const char *flags_str, int flags_len) {
    if (!flags_str)
        return XR_RE_NONE;
    if (flags_len < 0)
        flags_len = (int) strlen(flags_str);
    return (XrRegexFlags) xr_regex_core_parse_flags(flags_str, (size_t) flags_len);
}

/*
 * Create a RegexMatch instance (typed XrInstance, not Json).
 *
 * Fields (fixed slot offsets, direct store — no hash lookup):
 *   slot 0: start  (int)
 *   slot 1: end    (int)
 *   slot 2: text   (string)
 *   slot 3: groups (Array<string>)
 */
XrValue xr_regex_make_match_object(XrVMRuntime *isolate, const char *text, XrMatch *match) {
    XrayCoreClasses *core = xr_isolate_get_core_classes(isolate);
    XR_DCHECK(core && core->regexMatchClass, "make_match_object: regexMatchClass not registered");

    /* Temporarily disable GC: multiple allocations below (Instance,
     * String, Array) are not rooted from the VM stack. */
    XrCoroutine *coro = xr_current_coro(isolate);
    XrCoroHeap *heap = coro ? coro->heap : NULL;
    if (heap)
        heap->cycle_collection_disabled++;

    XrInstance *inst = xr_instance_new(isolate, core->regexMatchClass);
    XR_DCHECK(inst != NULL, "make_match_object: instance alloc failed");

    /* start / end */
    int start_offset = match->groups[0].start ? (int) (match->groups[0].start - text) : 0;
    int end_offset = match->groups[0].end ? (int) (match->groups[0].end - text) : 0;
    xr_instance_set_field_fast(inst, XR_REGEX_CORE_MATCH_START, xr_int(start_offset));
    xr_instance_set_field_fast(inst, XR_REGEX_CORE_MATCH_END, xr_int(end_offset));

    /* text */
    if (match->groups[0].start && match->groups[0].end) {
        int len = (int) (match->groups[0].end - match->groups[0].start);
        XrString *matched_text = xr_string_intern(isolate, match->groups[0].start, len, 0);
        xr_instance_set_field_fast(inst, XR_REGEX_CORE_MATCH_TEXT, xr_string_value(matched_text));
    } else {
        xr_instance_set_field_fast(inst, XR_REGEX_CORE_MATCH_TEXT, xr_null());
    }

    /* groups */
    XrArray *groups = xr_array_new(coro);
    for (int i = 0; i < match->group_count; i++) {
        if (match->groups[i].start && match->groups[i].end) {
            int len = (int) (match->groups[i].end - match->groups[i].start);
            XrString *group_text = xr_string_intern(isolate, match->groups[i].start, len, 0);
            xr_array_push(groups, xr_string_value(group_text));
        } else {
            xr_array_push(groups, xr_null());
        }
    }
    xr_instance_set_field_fast(inst, XR_REGEX_CORE_MATCH_GROUPS, xr_value_from_array(groups));

    if (heap)
        heap->cycle_collection_disabled--;
    return XR_FROM_PTR(inst);
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
static XrValue wrap_regex(XrVMRuntime *isolate, XrRegex *re) {
    XrayCoreClasses *core = xr_isolate_get_core_classes(isolate);
    XR_DCHECK(core && core->regexClass, "wrap_regex: regexClass not registered");
    /* xr_instance_new takes XrVMRuntime*, not XrCoroutine*; passing coro
     * here was type-confusion that caused module-init regex.compile to
     * dereference garbage when no coroutine was running yet (multi-module
     * preload phase before VM start). xr_instance_new internally resolves
     * the current coro via xr_current_coro(isolate). */
    XrInstance *inst = xr_instance_new(isolate, core->regexClass);
    if (!inst)
        return xr_null();
    RegexBody *body = regex_body(inst);
    body->regex = re;
    return XR_FROM_PTR(inst);
}

/* Unwrap XrRegex* from an XrValue */
static XrRegex *unwrap_regex(XrVMRuntime *isolate, XrValue v) {
    (void) isolate;
    if (!is_regex_instance(v))
        return NULL;
    XrInstance *inst = (XrInstance *) XR_TO_PTR(v);
    return regex_body(inst)->regex;
}

/* Public API: wrap XrRegex as XrValue */
XrValue xr_regex_wrap(XrVMRuntime *isolate, XrRegex *re) {
    return wrap_regex(isolate, re);
}

// Public API: compile a regex literal (OP_REGEX_COMPILE bytecode helper).
// Both arguments must be strings; flag chars 'i' / 'm' / 's' map to
// XR_RE_IGNORECASE / MULTILINE / DOTALL, anything else is silently
// ignored to preserve the inline-parser behavior the VM dispatch had
// before the bridge existed.
XrValue xr_regex_compile_literal(XrVMRuntime *isolate, XrValue pattern_val, XrValue flags_val) {
    XrString *pattern_str = xr_value_to_string(isolate, pattern_val);
    XrString *flags_str = xr_value_to_string(isolate, flags_val);
    if (!pattern_str || !flags_str) {
        return xr_null();
    }

    XrRegexFlags regex_flags = XR_RE_NONE;
    regex_flags = parse_flags(flags_str->data, (int) flags_str->length);

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
static XrValue regex_compile(XrVMRuntime *isolate, XrValue *args, int argc) {
    if (argc < 1)
        return xr_null();

    int pattern_len;
    const char *pattern = value_to_cstring(args[0], &pattern_len);
    if (!pattern)
        return xr_null();

    XrRegexFlags flags = XR_RE_NONE;
    if (argc >= 2) {
        int flags_len;
        const char *flags_str = value_to_cstring(args[1], &flags_len);
        flags = parse_flags(flags_str, flags_len);
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
static XrValue regex_test(XrVMRuntime *isolate, XrValue *args, int argc) {
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
static XrValue regex_full_match(XrVMRuntime *isolate, XrValue *args, int argc) {
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
static XrValue regex_count(XrVMRuntime *isolate, XrValue *args, int argc) {
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

// findText(re, text) - Find match, return only matched text (zero-alloc)
static XrValue regex_find_text(XrVMRuntime *isolate, XrValue *args, int argc) {
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
    if (!xr_regex_match(re, text, text_len, &match))
        return xr_null();
    if (!match.groups[0].start || !match.groups[0].end)
        return xr_null();

    int len = (int) (match.groups[0].end - match.groups[0].start);
    return xr_string_value(xr_string_intern(isolate, match.groups[0].start, len, 0));
}

// findGroup(re, text, index) - Find match, return single capture group (zero-alloc)
static XrValue regex_find_group(XrVMRuntime *isolate, XrValue *args, int argc) {
    if (argc < 3)
        return xr_null();

    XrRegex *re = unwrap_regex(isolate, args[0]);
    if (!re)
        return xr_null();

    int text_len;
    const char *text = value_to_cstring(args[1], &text_len);
    if (!text)
        return xr_null();

    if (!XR_IS_INT(args[2]))
        return xr_null();
    int group_idx = 0;
    if (!xr_regex_core_int_arg(XR_TO_INT(args[2]), &group_idx))
        return xr_null();

    XrMatch match;
    if (!xr_regex_match(re, text, text_len, &match))
        return xr_null();

    if (group_idx < 0 || group_idx >= match.group_count)
        return xr_null();
    if (!match.groups[group_idx].start || !match.groups[group_idx].end)
        return xr_null();

    int len = (int) (match.groups[group_idx].end - match.groups[group_idx].start);
    return xr_string_value(xr_string_intern(isolate, match.groups[group_idx].start, len, 0));
}

// find(re, text [, offset]) - Find match from specified position
static XrValue regex_find(XrVMRuntime *isolate, XrValue *args, int argc) {
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
        if (!xr_regex_core_int_arg(XR_TO_INT(args[2]), &offset))
            return xr_null();
    }

    XrMatch match;
    bool found = xr_regex_match_at(re, text, text_len, offset, &match);
    if (!found)
        return xr_null();

    return xr_regex_make_match_object(isolate, text, &match);
}

// findAll(re, text [, limit]) - Find all matches
static XrValue regex_find_all(XrVMRuntime *isolate, XrValue *args, int argc) {
    if (argc < 2)
        return xr_value_from_array(xr_array_new(xr_current_coro(isolate)));

    XrRegex *re = unwrap_regex(isolate, args[0]);
    if (!re)
        return xr_value_from_array(xr_array_new(xr_current_coro(isolate)));

    int text_len;
    const char *text = value_to_cstring(args[1], &text_len);
    if (!text)
        return xr_value_from_array(xr_array_new(xr_current_coro(isolate)));

    int limit = xr_regex_core_limit_arg(argc >= 3 && XR_IS_INT(args[2]),
                                        (argc >= 3 && XR_IS_INT(args[2])) ? XR_TO_INT(args[2]) : 0);

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
static XrValue regex_replace(XrVMRuntime *isolate, XrValue *args, int argc) {
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
static XrValue regex_replace_all(XrVMRuntime *isolate, XrValue *args, int argc) {
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
static XrValue regex_split(XrVMRuntime *isolate, XrValue *args, int argc) {
    if (argc < 2)
        return xr_value_from_array(xr_array_new(xr_current_coro(isolate)));

    XrRegex *re = unwrap_regex(isolate, args[0]);
    if (!re)
        return xr_value_from_array(xr_array_new(xr_current_coro(isolate)));

    int text_len;
    const char *text = value_to_cstring(args[1], &text_len);
    if (!text)
        return xr_value_from_array(xr_array_new(xr_current_coro(isolate)));

    int limit = xr_regex_core_limit_arg(argc >= 3 && XR_IS_INT(args[2]),
                                        (argc >= 3 && XR_IS_INT(args[2])) ? XR_TO_INT(args[2]) : 0);

    // Dynamic allocation to avoid stack overflow on large inputs
    int max_parts = xr_regex_core_split_max_parts(limit);
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
static XrValue regex_escape(XrVMRuntime *isolate, XrValue *args, int argc) {
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
static XrValue regex_is_valid(XrVMRuntime *isolate, XrValue *args, int argc) {
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
static XrValue re_method_pattern(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
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

static XrValue re_m_test(XrVMRuntime *X, XrValue self, XrValue *a, int n) {
    XrValue tmp[3] = {self, n > 0 ? a[0] : xr_null(), n > 1 ? a[1] : xr_null()};
    return regex_test(X, tmp, n + 1);
}
static XrValue re_m_find(XrVMRuntime *X, XrValue self, XrValue *a, int n) {
    XrValue tmp[4] = {self, n > 0 ? a[0] : xr_null(), n > 1 ? a[1] : xr_null()};
    return regex_find(X, tmp, n + 1);
}
static XrValue re_m_find_all(XrVMRuntime *X, XrValue self, XrValue *a, int n) {
    XrValue tmp[4] = {self, n > 0 ? a[0] : xr_null(), n > 1 ? a[1] : xr_null()};
    return regex_find_all(X, tmp, n + 1);
}
static XrValue re_m_replace(XrVMRuntime *X, XrValue self, XrValue *a, int n) {
    XrValue tmp[4] = {self, n > 0 ? a[0] : xr_null(), n > 1 ? a[1] : xr_null()};
    return regex_replace(X, tmp, n + 1);
}
static XrValue re_m_replace_all(XrVMRuntime *X, XrValue self, XrValue *a, int n) {
    XrValue tmp[4] = {self, n > 0 ? a[0] : xr_null(), n > 1 ? a[1] : xr_null()};
    return regex_replace_all(X, tmp, n + 1);
}
static XrValue re_m_split(XrVMRuntime *X, XrValue self, XrValue *a, int n) {
    XrValue tmp[4] = {self, n > 0 ? a[0] : xr_null(), n > 1 ? a[1] : xr_null()};
    return regex_split(X, tmp, n + 1);
}

/* findText: zero-allocation path — return only the matched text (string?).
 * No RegexMatch object, no groups array allocated. */
static XrValue re_m_find_text(XrVMRuntime *X, XrValue self, XrValue *a, int n) {
    if (n < 1)
        return xr_null();
    XrRegex *re = unwrap_regex(X, self);
    if (!re)
        return xr_null();
    int text_len;
    const char *text = value_to_cstring(a[0], &text_len);
    if (!text)
        return xr_null();

    XrMatch match;
    if (!xr_regex_match(re, text, text_len, &match))
        return xr_null();

    if (!match.groups[0].start || !match.groups[0].end)
        return xr_null();

    int len = (int) (match.groups[0].end - match.groups[0].start);
    return xr_string_value(xr_string_intern(X, match.groups[0].start, len, 0));
}

/* findGroup: zero-allocation path — return a single capture group (string?).
 * args: (text: string, index: int). index 0 = whole match, 1+ = groups. */
static XrValue re_m_find_group(XrVMRuntime *X, XrValue self, XrValue *a, int n) {
    if (n < 2)
        return xr_null();
    XrRegex *re = unwrap_regex(X, self);
    if (!re)
        return xr_null();
    int text_len;
    const char *text = value_to_cstring(a[0], &text_len);
    if (!text)
        return xr_null();
    if (!XR_IS_INT(a[1]))
        return xr_null();
    int group_idx = (int) XR_TO_INT(a[1]);

    XrMatch match;
    if (!xr_regex_match(re, text, text_len, &match))
        return xr_null();

    if (group_idx < 0 || group_idx >= match.group_count)
        return xr_null();
    if (!match.groups[group_idx].start || !match.groups[group_idx].end)
        return xr_null();

    int len = (int) (match.groups[group_idx].end - match.groups[group_idx].start);
    return xr_string_value(xr_string_intern(X, match.groups[group_idx].start, len, 0));
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

static XrValue re_m_to_string(XrVMRuntime *iso, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    return xr_string_value(xr_value_to_string(iso, self));
}

#define XR_STDLIB_VM_BIND_CLASS_REGEX 1
#define XR_STDLIB_VM_BIND_CLASS_REGEX_MATCH 1
#include "../../src/stdlib/xstdlib_class_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_CLASS_REGEX_MATCH
#undef XR_STDLIB_VM_BIND_CLASS_REGEX

void xr_regex_register_class(XrVMRuntime *isolate) {
    xr_stdlib_vm_register_regex_class_generated(isolate);
    xr_stdlib_vm_register_regex_match_class_generated(isolate);
}

/* ========================================================================
 * Module Loading
 * ======================================================================== */

#define XR_STDLIB_VM_BIND_MODULE_REGEX 1
#include "../../src/stdlib/xstdlib_vm_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_MODULE_REGEX

XR_FUNC XrModule *xr_load_module_regex(XrVMRuntime *isolate) {
    // 1. Create native module
    XrModule *mod = xr_module_create_native(isolate, "regex");
    if (!mod)
        return NULL;

    xr_stdlib_vm_bind_regex_generated(isolate, mod);

    // The Regex XrClass itself is registered up front by the prelude
    // module — no need to do it again here.
    mod->loaded = true;
    return mod;
}
