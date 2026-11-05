/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmodule_fingerprint.c - One domain-separated source identity algorithm
 */
#include "xmodule_fingerprint.h"
#include "../base/xsha256.h"
#include <string.h>

XR_FUNC void xr_module_source_fingerprint(const char *source, XrFingerprint *output) {
    if (!output) return;
    if (!source) source = "";
    static const uint8_t domain[] = "xray-module-source-v1\0";
    size_t length = strlen(source);
    uint8_t length_bytes[8];
    for (uint32_t i = 0; i < sizeof(length_bytes); ++i)
        length_bytes[i] = (uint8_t)((uint64_t)length >> (i * 8u));
    XrSHA256Context hash; xr_sha256_init(&hash);
    xr_sha256_update(&hash, domain, sizeof(domain) - 1);
    xr_sha256_update(&hash, length_bytes, sizeof(length_bytes));
    if (length) xr_sha256_update(&hash, (const uint8_t *)source, length);
    XrFingerprint result;
    xr_sha256_final(&hash, result.bytes);
    *output = result;
}
