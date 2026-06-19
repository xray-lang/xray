/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_regex_core.c - Freestanding AOT regex engine helpers
 */

#include "../../stdlib/regex/xregex_internal.h"
#include <stdbool.h>
#include <stdint.h>

bool xrt_regex_is_valid_core(const char *data, int64_t len) {
    if (!data && len != 0)
        return false;

    if (len < 0)
        len = data ? (int64_t) strlen(data) : 0;
    if (len < 0)
        return false;

    char *pattern = (char *) xr_re_alloc((size_t) len + 1);
    if (!pattern)
        return false;
    if (len > 0)
        memcpy(pattern, data, (size_t) len);
    pattern[len] = '\0';

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
