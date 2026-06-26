/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_cstr_core.h - Runtime-neutral C string argument helpers.
 */

#ifndef XR_CSTR_CORE_H
#define XR_CSTR_CORE_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef void *(*XrCStrCoreAllocFn)(void *ctx, size_t size);

static inline char *xr_cstr_core_copy_arg(const char *data, int64_t len, char *stack,
                                          size_t stack_cap, XrCStrCoreAllocFn alloc_fn,
                                          void *alloc_ctx, char **owned_out) {
    if (owned_out)
        *owned_out = NULL;
    if (!data || len < 0 || !stack || stack_cap == 0)
        return NULL;

    uint64_t n64 = (uint64_t) len;
    if (n64 > (uint64_t) SIZE_MAX - 1)
        return NULL;

    size_t n = (size_t) n64;
    char *out = stack;
    if (n + 1 > stack_cap) {
        if (!alloc_fn)
            return NULL;
        out = (char *) alloc_fn(alloc_ctx, n + 1);
        if (!out)
            return NULL;
        if (owned_out)
            *owned_out = out;
    }

    memcpy(out, data, n);
    out[n] = '\0';
    return out;
}

#endif /* XR_CSTR_CORE_H */
