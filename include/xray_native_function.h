/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_native_function.h - Fixed storage for admitted callable bindings
 *
 * KEY CONCEPT:
 *   Entry descriptions and borrowed environments are protected by their real
 *   lifetimes. Storage describes neither invocation nor escaping permission.
 */
#ifndef XRAY_NATIVE_FUNCTION_H
#define XRAY_NATIVE_FUNCTION_H
#include <stddef.h>
#include <stdint.h>

typedef struct XrtManagedHeader XrtManagedHeader;

typedef struct XrtFunction {
    /* An immutable backend entry object, never a converted function pointer. */
    const void *entry;
    /* Native stack address or VM environment index, interpreted by the entry. */
    uintptr_t environment;
    /* NULL for Frame borrows; escaping values own an Env with a code lease. */
    XrtManagedHeader *owner;
} XrtFunction;

#if UINTPTR_MAX == UINT64_MAX
#ifdef __cplusplus
static_assert(sizeof(XrtFunction) == 24 && alignof(XrtFunction) == 8 &&
    offsetof(XrtFunction, entry) == 0 && offsetof(XrtFunction, environment) == 8 &&
    offsetof(XrtFunction, owner) == 16, "callable storage follows the native C layout");
#else
_Static_assert(sizeof(XrtFunction) == 24 && _Alignof(XrtFunction) == 8 &&
    offsetof(XrtFunction, entry) == 0 && offsetof(XrtFunction, environment) == 8 &&
    offsetof(XrtFunction, owner) == 16, "callable storage follows the native C layout");
#endif
#endif
#endif // XRAY_NATIVE_FUNCTION_H
