/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * raw_keys.c - Allocation and byte hashing for bare C strings
 *
 * KEY CONCEPT:
 *   Separate compilation keeps original temporary-string work in both controls.
 */
#include "raw.h"

XR_FUNCDEF XrMvlRawString *xr_mvl_raw_string_new(const char *bytes, size_t length) {
    if ((!bytes && length) || length > SIZE_MAX - sizeof(XrMvlRawString) - 1)
        return NULL;
    XrMvlRawString *string = xr_malloc(sizeof(*string) + length + 1);
    if (!string)
        return NULL;
    string->length = length;
    string->hash = xr_hash_bytes64(bytes, length);
    if (length)
        memcpy(string->bytes, bytes, length);
    string->bytes[length] = '\0';
    return string;
}
