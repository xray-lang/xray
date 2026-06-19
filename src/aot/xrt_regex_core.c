/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_regex_core.c - Freestanding AOT regex engine helpers
 */

#include "xrt_regex.h"
#include "../../stdlib/regex/xregex.h"
#include "../../stdlib/regex/xregex_internal.h"
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>

static char *xrt_regex_copy_cstring(const char *data, int64_t len) {
    if (!data && len != 0)
        return NULL;
    if (len < 0)
        len = data ? (int64_t) strlen(data) : 0;
    if (len < 0)
        return NULL;
    char *copy = (char *) xr_re_alloc((size_t) len + 1);
    if (!copy)
        return NULL;
    if (len > 0)
        memcpy(copy, data, (size_t) len);
    copy[len] = '\0';
    return copy;
}

static XrRegexFlags xrt_regex_parse_flags(const char *data, int64_t len) {
    XrRegexFlags flags = XR_RE_NONE;
    if (!data && len != 0)
        return flags;
    if (len < 0)
        len = data ? (int64_t) strlen(data) : 0;
    for (int64_t i = 0; i < len; i++) {
        switch (data[i]) {
            case 'i':
                flags |= XR_RE_IGNORECASE;
                break;
            case 'm':
                flags |= XR_RE_MULTILINE;
                break;
            case 's':
                flags |= XR_RE_DOTALL;
                break;
            default:
                break;
        }
    }
    return flags;
}

bool xrt_regex_is_valid_core(const char *data, int64_t len) {
    if (!data && len != 0)
        return false;

    char *pattern = xrt_regex_copy_cstring(data, len);
    if (!pattern)
        return false;

    XrParser parser;
    XrAstNode *ast = xr_regex_parse(pattern, XR_RE_NONE, &parser);
    bool valid = ast != NULL && parser.error == XR_RE_OK;
    xr_arena_destroy(&parser.ast_arena);
    if (parser.capture_names) {
        for (int i = 0; i < parser.names_count; i++)
            xr_re_free(parser.capture_names[i]);
        xr_re_free(parser.capture_names);
    }
    xr_re_free(pattern);
    return valid;
}

XrValue xrt_regex_compile_with_flags(const char *pattern_data, int64_t pattern_len,
                                     const char *flags_data, int64_t flags_len) {
    char *pattern = xrt_regex_copy_cstring(pattern_data, pattern_len);
    if (!pattern)
        return XR_NULL_VAL;

    XrRegexError error;
    XrRegexFlags flags = xrt_regex_parse_flags(flags_data, flags_len);
    XrRegex *re = xr_regex_compile(pattern, flags, &error);
    xr_re_free(pattern);
    if (!re)
        return XR_NULL_VAL;

    xrt_regex_object_t *obj = (xrt_regex_object_t *) xrt_arc_alloc(sizeof(xrt_regex_object_t));
    if (!obj) {
        xr_regex_free(re);
        return XR_NULL_VAL;
    }
    xrt_arc_mark_builtin(obj, XRT_ARC_KIND_REGEX);
    obj->regex = re;
    return xr_mkptr(obj, XR_TAG_REGEX);
}

XrValue xrt_regex_compile_default(const char *pattern_data, int64_t pattern_len) {
    return xrt_regex_compile_with_flags(pattern_data, pattern_len, NULL, 0);
}

XrValue xrt_regex_test(XrValue re_value, const char *text_data, int64_t text_len) {
    if (re_value.tag != XR_TAG_REGEX || !re_value.ptr || !text_data)
        return XR_FALSE_VAL;
    if (text_len < 0)
        text_len = (int64_t) strlen(text_data);
    if (text_len < 0 || text_len > (int64_t) INT_MAX)
        return XR_FALSE_VAL;

    xrt_regex_object_t *obj = (xrt_regex_object_t *) re_value.ptr;
    return xr_regex_test(obj->regex, text_data, (int) text_len) ? XR_TRUE_VAL : XR_FALSE_VAL;
}
