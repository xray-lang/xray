/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_identity_work.h - Private work-before-operation identity primitives
 */
#ifndef XR_IDENTITY_WORK_H
#define XR_IDENTITY_WORK_H
#include "../base/xcompile_resources.h"
#include "../base/xsha256.h"
#include <string.h>

typedef struct XrIdentityWork { XrCompileResources *resources; bool failed; } XrIdentityWork;
static inline bool identity_work(XrIdentityWork *work, size_t units) {
    if (work->failed) return false;
    if (xr_compile_resources_work(work->resources, (uint64_t)units) != XR_COMPILE_RESOURCE_OK) {
        work->failed = true;
        return false;
    }
    return true;
}
static inline bool identity_copy(XrIdentityWork *work, void *out, const void *in, size_t bytes) {
    if (!identity_work(work, bytes)) return false;
    memcpy(out, in, bytes);
    return true;
}
static inline bool identity_zero(XrIdentityWork *work, void *out, size_t bytes) {
    if (!identity_work(work, bytes)) return false;
    memset(out, 0, bytes);
    return true;
}
static inline bool identity_nonzero(XrIdentityWork *work, const uint8_t *bytes) {
    uint8_t bits = 0;
    for (unsigned i = 0; i < 32; ++i) {
        if (!identity_work(work, 1)) return false;
        bits |= bytes[i];
    }
    return bits != 0;
}
static inline bool identity_same(XrIdentityWork *work, const uint8_t *a, const uint8_t *b) {
    for (unsigned i = 0; i < 32; ++i) {
        if (!identity_work(work, 2)) return false;
        if (a[i] != b[i]) return false;
    }
    return true;
}
static inline bool identity_begin(XrIdentityWork *work, XrSHA256Context *hash) {
    if (!identity_work(work, 1)) return false;
    xr_sha256_init(hash);
    return true;
}
static inline bool identity_bytes(XrIdentityWork *work, XrSHA256Context *hash,
    const void *bytes, size_t count) {
    if (!identity_work(work, count)) return false;
    xr_sha256_update(hash, bytes, count);
    return true;
}
static inline bool identity_integer(XrIdentityWork *work, XrSHA256Context *hash,
    uint64_t value, unsigned count) {
    uint8_t bytes[8];
    if (!identity_work(work, count)) return false;
    for (unsigned i = 0; i < count; ++i) bytes[i] = (uint8_t)(value >> (8u * i));
    return identity_bytes(work, hash, bytes, count);
}
static inline bool identity_end(XrIdentityWork *work, XrSHA256Context *hash, uint8_t *output) {
    if (!identity_work(work, 33)) return false;
    xr_sha256_final(hash, output);
    return true;
}
#endif // XR_IDENTITY_WORK_H
