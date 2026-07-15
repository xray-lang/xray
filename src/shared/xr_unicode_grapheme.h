/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * Unicode 17.0.0 UAX #29 grapheme cursor shared by VM and AOT.
 */

#ifndef XR_UNICODE_GRAPHEME_H
#define XR_UNICODE_GRAPHEME_H

#include "xr_unicode_grapheme_data.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum XrGraphemeRule {
    XR_GRAPHEME_RULE_NONE = 0,
    XR_GRAPHEME_RULE_GB1,
    XR_GRAPHEME_RULE_GB2,
    XR_GRAPHEME_RULE_GB3,
    XR_GRAPHEME_RULE_GB4,
    XR_GRAPHEME_RULE_GB5,
    XR_GRAPHEME_RULE_GB6,
    XR_GRAPHEME_RULE_GB7,
    XR_GRAPHEME_RULE_GB8,
    XR_GRAPHEME_RULE_GB9,
    XR_GRAPHEME_RULE_GB9A,
    XR_GRAPHEME_RULE_GB9B,
    XR_GRAPHEME_RULE_GB9C,
    XR_GRAPHEME_RULE_GB11,
    XR_GRAPHEME_RULE_GB12_13,
    XR_GRAPHEME_RULE_GB999,
} XrGraphemeRule;

typedef struct XrByteRange {
    size_t start;
    size_t end;
} XrByteRange;

typedef struct XrGraphemeCursor {
    const uint8_t *data;
    size_t length;
    size_t offset;
    uint32_t state;
    XrGraphemeRule last_rule;
} XrGraphemeCursor;

typedef void (*XrGraphemeTraceFn)(size_t offset, bool is_break, XrGraphemeRule rule, void *context);

XR_FUNC void xr_grapheme_cursor_init(XrGraphemeCursor *cursor, const uint8_t *data, size_t length);
XR_FUNC bool xr_grapheme_cursor_next(XrGraphemeCursor *cursor, XrByteRange *out);
XR_FUNC bool xr_grapheme_cursor_next_traced(XrGraphemeCursor *cursor, XrByteRange *out,
                                            XrGraphemeTraceFn trace, void *context);
XR_FUNC const char *xr_grapheme_rule_name(XrGraphemeRule rule);

#endif /* XR_UNICODE_GRAPHEME_H */
