/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstring_hash.c - Small standalone string hashing entry point.
 */

#include "xstring.h"
#include "../../base/xhash.h"

uint32_t xr_string_hash(const char *chars, size_t length) {
    return xr_hash_bytes(chars, length);
}
