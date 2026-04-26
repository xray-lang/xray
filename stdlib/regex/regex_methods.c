/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * regex_methods.c - Regex instance method bodies + dispatch table.
 *
 * Each XrMethodSlot wrapper takes the receiver as XrValue self and the
 * remaining positional arguments as args / argc, mirroring
 * stdlib/datetime/datetime_methods.c. The bodies dispatch into the
 * public xr_regex_* engine API so the slot table stays a thin adapter
 * layer.
 */

#include "regex_methods.h"
#include "xregex.h"
#include "xregex_binding.h"
#include "../../src/runtime/object/xstring.h"
#include "../../src/runtime/object/xarray.h"
#include "../../src/runtime/object/xjson.h"
#include "../../src/runtime/value/xvalue.h"
#include "../../src/runtime/symbol/xsymbol_table.h"
#include "../../src/base/xchecks.h"
#include "../../src/base/xmalloc.h"
#include <string.h>

/* ========== Helpers ========== */

static inline XrRegex *re_self(XrValue self) {
    XrRegex *re = xr_value_to_regex(self);
    XR_DCHECK(re != NULL, "regex method: receiver is not a Regex");
    return re;
}

// Extract a (data, len) pair from an XrValue string. Returns NULL data
// on type mismatch -- callers must short-circuit so we never feed
// garbage into the regex engine.
static inline const char *value_text(XrValue v, int *len) {
    if (XR_IS_STRING(v)) {
        XrString *s = XR_TO_STRING(v);
        *len = (int)s->length;
        return s->data;
    }
    *len = 0;
    return NULL;
}

/* ========== Method bodies ========== */

// regex.test(text) -> bool
static XrValue m_test(XrayIsolate *iso, XrValue self,
                      XrValue *args, int argc) {
    (void)iso;
    if (argc < 1) return XR_FALSE_VAL;
    int tlen = 0;
    const char *text = value_text(args[0], &tlen);
    if (!text) return XR_FALSE_VAL;
    return xr_regex_test(re_self(self), text, tlen) ? XR_TRUE_VAL : XR_FALSE_VAL;
}

// regex.find(text [, offset]) -> Json|null
static XrValue m_find(XrayIsolate *iso, XrValue self,
                      XrValue *args, int argc) {
    if (argc < 1) return xr_null();
    int tlen = 0;
    const char *text = value_text(args[0], &tlen);
    if (!text) return xr_null();
    int offset = 0;
    if (argc >= 2 && XR_IS_INT(args[1])) {
        offset = (int)XR_TO_INT(args[1]);
    }
    XrMatch match;
    if (!xr_regex_match_at(re_self(self), text, tlen, offset, &match)) {
        return xr_null();
    }
    return xr_regex_make_match_object(iso, text, &match);
}

// regex.findAll(text [, limit]) -> Array<Json>
static XrValue m_find_all(XrayIsolate *iso, XrValue self,
                          XrValue *args, int argc) {
    XrArray *out = xr_array_new(xr_current_coro(iso));
    if (argc < 1) return xr_value_from_array(out);
    int tlen = 0;
    const char *text = value_text(args[0], &tlen);
    if (!text) return xr_value_from_array(out);
    int limit = -1;
    if (argc >= 2 && XR_IS_INT(args[1])) {
        limit = (int)XR_TO_INT(args[1]);
    }
    int count = 0;
    XrMatch *matches = xr_regex_find_all(re_self(self), text, tlen, limit, &count);
    if (matches) {
        for (int i = 0; i < count; i++) {
            xr_array_push(out, xr_regex_make_match_object(iso, text, &matches[i]));
        }
        xr_regex_find_all_free(matches);
    }
    return xr_value_from_array(out);
}

static XrValue regex_replace_common(XrayIsolate *iso, XrValue self,
                                    XrValue *args, int argc, bool replace_all) {
    if (argc < 2) return self;  // nothing to replace, return receiver
    int tlen = 0;
    const char *text = value_text(args[0], &tlen);
    if (!text) return self;
    if (!XR_IS_STRING(args[1])) return self;
    const char *repl = XR_TO_STRING(args[1])->data;

    char *out = xr_regex_replace_alloc(re_self(self), text, tlen,
                                        repl, replace_all);
    if (!out) {
        // No matches -> engine returned NULL; emit original text.
        return xr_string_value(xr_string_intern(iso, text, (size_t)tlen, 0));
    }
    XrString *result = xr_string_intern(iso, out, strlen(out), 0);
    xr_free_raw(out);
    return xr_string_value(result);
}

// regex.replace(text, repl) -> string
static XrValue m_replace(XrayIsolate *iso, XrValue self,
                         XrValue *args, int argc) {
    return regex_replace_common(iso, self, args, argc, false);
}

// regex.replaceAll(text, repl) -> string
static XrValue m_replace_all(XrayIsolate *iso, XrValue self,
                             XrValue *args, int argc) {
    return regex_replace_common(iso, self, args, argc, true);
}

// regex.split(text [, limit]) -> Array<string>
static XrValue m_split(XrayIsolate *iso, XrValue self,
                       XrValue *args, int argc) {
    XrArray *out = xr_array_new(xr_current_coro(iso));
    if (argc < 1) return xr_value_from_array(out);
    int tlen = 0;
    const char *text = value_text(args[0], &tlen);
    if (!text) return xr_value_from_array(out);

    int limit = -1;
    if (argc >= 2 && XR_IS_INT(args[1])) {
        limit = (int)XR_TO_INT(args[1]);
    }

    int max_parts = (limit > 0 ? limit : tlen + 1);
    XrSplitPart *parts = (XrSplitPart *)xr_malloc(
        (size_t)max_parts * sizeof(XrSplitPart));
    if (!parts) return xr_value_from_array(out);

    int n = xr_regex_split(re_self(self), text, tlen,
                           parts, max_parts, limit);
    for (int i = 0; i < n; i++) {
        XrString *piece = xr_string_intern(iso, parts[i].str,
                                           (size_t)parts[i].len, 0);
        xr_array_push(out, xr_string_value(piece));
    }
    xr_free(parts);
    return xr_value_from_array(out);
}

// regex.pattern -> string (the source pattern that compiled this regex)
static XrValue m_pattern(XrayIsolate *iso, XrValue self,
                         XrValue *args, int argc) {
    (void)args; (void)argc;
    const char *pat = xr_regex_pattern(re_self(self));
    if (!pat) return xr_null();
    return xr_string_value(xr_string_intern(iso, pat, strlen(pat), 0));
}

/* ========== Slot table ========== */

#define MAY_THROW XR_METHOD_FLAG_MAY_THROW

const XrMethodSlot xr_regex_method_table[SYMBOL_BUILTIN_COUNT] = {
    [SYMBOL_TEST]       = { m_test,        1, 1, XR_METHOD_FLAG_PURE },
    [SYMBOL_FIND]       = { m_find,        1, 2, MAY_THROW },
    [SYMBOL_FINDALL]    = { m_find_all,    1, 2, MAY_THROW },
    [SYMBOL_REPLACE]    = { m_replace,     2, 2, MAY_THROW },
    [SYMBOL_REPLACEALL] = { m_replace_all, 2, 2, MAY_THROW },
    [SYMBOL_SPLIT]      = { m_split,       1, 2, MAY_THROW },
    [SYMBOL_PATTERN]    = { m_pattern,     0, 0, XR_METHOD_FLAG_PURE },
};

#undef MAY_THROW
