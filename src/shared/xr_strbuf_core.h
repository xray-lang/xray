/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_strbuf_core.h - Runtime-neutral StringBuilder append literal rules.
 */

#ifndef XR_STRBUF_CORE_H
#define XR_STRBUF_CORE_H

#include <stdbool.h>
#include <stddef.h>

typedef struct XrStrbufCoreSlice {
    const char *data;
    size_t len;
} XrStrbufCoreSlice;

typedef enum XrStrbufCoreLiteralKind {
    XR_STRBUF_CORE_LITERAL_BOOL = 0,
    XR_STRBUF_CORE_LITERAL_NULL,
    XR_STRBUF_CORE_LITERAL_OBJECT,
} XrStrbufCoreLiteralKind;

static inline XrStrbufCoreSlice xr_strbuf_core_literal_slice(XrStrbufCoreLiteralKind kind,
                                                             bool bool_value) {
    switch (kind) {
        case XR_STRBUF_CORE_LITERAL_BOOL:
            return bool_value ? (XrStrbufCoreSlice) {"true", 4} : (XrStrbufCoreSlice) {"false", 5};
        case XR_STRBUF_CORE_LITERAL_NULL:
            return (XrStrbufCoreSlice) {"null", 4};
        case XR_STRBUF_CORE_LITERAL_OBJECT:
            return (XrStrbufCoreSlice) {"<object>", 8};
    }
    return (XrStrbufCoreSlice) {"", 0};
}

#endif /* XR_STRBUF_CORE_H */
