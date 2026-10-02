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

typedef struct SourceFingerprintWork {
    void *context;
    bool (*charge)(void *context, uint64_t units);
} SourceFingerprintWork;
static bool fingerprint_unmetered(void *context, uint64_t units) {
    (void)context; (void)units; return true;
}
static bool source_fingerprint(const SourceFingerprintWork *work, const char *source, XrFingerprint *output) {
    static const uint8_t domain[] = "xray-module-source-v1\0";
    size_t length = 0;
    for (;;) {
        if (!work->charge(work->context, 1)) return false;
        if (!source[length]) break;
        if (length == SIZE_MAX - 1) return false;
        ++length;
    }
    uint8_t length_bytes[8];
    if (!work->charge(work->context, sizeof(length_bytes))) return false;
    for (uint32_t i = 0; i < sizeof(length_bytes); ++i)
        length_bytes[i] = (uint8_t)((uint64_t)length >> (i * 8u));
    if (!work->charge(work->context, 1)) return false;
    XrSHA256Context hash; xr_sha256_init(&hash);
    if (!work->charge(work->context, sizeof(domain) - 1)) return false;
    xr_sha256_update(&hash, domain, sizeof(domain) - 1);
    if (!work->charge(work->context, sizeof(length_bytes))) return false;
    xr_sha256_update(&hash, length_bytes, sizeof(length_bytes));
    if (length) {
        if (!work->charge(work->context, length)) return false;
        xr_sha256_update(&hash, (const uint8_t *)source, length);
    }
    if (!work->charge(work->context, 1)) return false;
    XrFingerprint result;
    xr_sha256_final(&hash, result.bytes);
    if (!work->charge(work->context, sizeof(result))) return false;
    *output = result;
    return true;
}
XR_FUNC void xr_module_source_fingerprint(const char *source, XrFingerprint *output) {
    if (!output) return;
    SourceFingerprintWork work = {NULL, fingerprint_unmetered};
    (void)source_fingerprint(&work, source ? source : "", output);
}
static bool fingerprint_charge(void *context, uint64_t units) {
    return xr_compile_resources_work(context, units) == XR_COMPILE_RESOURCE_OK;
}
XR_FUNC XrCompileResourceStatus xr_compile_module_source_fingerprint(
    XrCompileResources *resources, const char *source, XrFingerprint *output) {
    if (!resources || !source || !output) return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    SourceFingerprintWork work = {resources, fingerprint_charge};
    return source_fingerprint(&work, source, output) ? XR_COMPILE_RESOURCE_OK : XR_COMPILE_RESOURCE_BUDGET;
}
