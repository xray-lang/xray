/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstrbuf_string.c - XrStrBuf conversion to interned XrString.
 */

#include "xstrbuf.h"
#include "object/xstring.h"

XrString *xr_strbuf_to_string(XrStrBuf *sb) {
    uint32_t hash = xr_string_hash(sb->data, sb->length);
    XrString *s = xr_string_intern(sb->X, sb->data, sb->length, hash);
    sb->length = 0;
    return s;
}
