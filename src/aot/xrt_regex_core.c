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
#include <string.h>

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

XrValue xrt_regex_count(XrValue re_value, const char *text_data, int64_t text_len) {
    if (re_value.tag != XR_TAG_REGEX || !re_value.ptr || !text_data)
        return XR_FROM_INT(0);
    if (text_len < 0)
        text_len = (int64_t) strlen(text_data);
    if (text_len < 0 || text_len > (int64_t) INT_MAX)
        return XR_FROM_INT(0);

    xrt_regex_object_t *obj = (xrt_regex_object_t *) re_value.ptr;
    return XR_FROM_INT((int64_t) xr_regex_count(obj->regex, text_data, (int) text_len));
}

static XrValue xrt_regex_match_group_to_string(const XrMatch *match, int group_index) {
    if (!match || group_index < 0 || group_index >= match->group_count)
        return XR_NULL_VAL;
    const char *start = match->groups[group_index].start;
    const char *end = match->groups[group_index].end;
    if (!start || !end || end < start)
        return XR_NULL_VAL;

    size_t len = (size_t) (end - start);
    XrValue result = xrt_str_alloc(len);
    if (len > 0)
        memcpy(xr_str_buf(result), start, len);
    return result;
}

XrValue xrt_regex_find_text(XrValue re_value, const char *text_data, int64_t text_len) {
    if (re_value.tag != XR_TAG_REGEX || !re_value.ptr || !text_data)
        return XR_NULL_VAL;
    if (text_len < 0)
        text_len = (int64_t) strlen(text_data);
    if (text_len < 0 || text_len > (int64_t) INT_MAX)
        return XR_NULL_VAL;

    xrt_regex_object_t *obj = (xrt_regex_object_t *) re_value.ptr;
    XrMatch match;
    if (!xr_regex_match(obj->regex, text_data, (int) text_len, &match))
        return XR_NULL_VAL;
    return xrt_regex_match_group_to_string(&match, 0);
}

XrValue xrt_regex_find_group(XrValue re_value, const char *text_data, int64_t text_len,
                             XrValue group_value) {
    if (!XR_IS_INT(group_value))
        return XR_NULL_VAL;
    int64_t group_i64 = XR_TO_INT(group_value);
    if (group_i64 < 0 || group_i64 > INT_MAX)
        return XR_NULL_VAL;
    if (re_value.tag != XR_TAG_REGEX || !re_value.ptr || !text_data)
        return XR_NULL_VAL;
    if (text_len < 0)
        text_len = (int64_t) strlen(text_data);
    if (text_len < 0 || text_len > (int64_t) INT_MAX)
        return XR_NULL_VAL;

    xrt_regex_object_t *obj = (xrt_regex_object_t *) re_value.ptr;
    XrMatch match;
    if (!xr_regex_match(obj->regex, text_data, (int) text_len, &match))
        return XR_NULL_VAL;
    return xrt_regex_match_group_to_string(&match, (int) group_i64);
}

static XrValue xrt_regex_replace_impl(XrValue re_value, const char *text_data, int64_t text_len,
                                      const char *replacement_data, int64_t replacement_len,
                                      bool all) {
    (void) replacement_len;
    if (re_value.tag != XR_TAG_REGEX || !re_value.ptr || !text_data || !replacement_data)
        return XR_NULL_VAL;
    if (text_len < 0)
        text_len = (int64_t) strlen(text_data);
    if (text_len < 0 || text_len > (int64_t) INT_MAX)
        return XR_NULL_VAL;

    xrt_regex_object_t *obj = (xrt_regex_object_t *) re_value.ptr;
    char *replaced =
        xr_regex_replace_alloc(obj->regex, text_data, (int) text_len, replacement_data, all);
    if (!replaced)
        return XR_NULL_VAL;

    size_t out_len = strlen(replaced);
    XrValue result = xrt_str_alloc(out_len);
    if (out_len > 0)
        memcpy(xr_str_buf(result), replaced, out_len);
    xr_re_free(replaced);
    return result;
}

XrValue xrt_regex_replace(XrValue re_value, const char *text_data, int64_t text_len,
                          const char *replacement_data, int64_t replacement_len) {
    return xrt_regex_replace_impl(re_value, text_data, text_len, replacement_data, replacement_len,
                                  false);
}

XrValue xrt_regex_replace_all(XrValue re_value, const char *text_data, int64_t text_len,
                              const char *replacement_data, int64_t replacement_len) {
    return xrt_regex_replace_impl(re_value, text_data, text_len, replacement_data, replacement_len,
                                  true);
}
