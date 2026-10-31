/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_buffer_storage.h - Owned private byte-storage payloads
 *
 * KEY CONCEPT:
 *   The allocation domain owns every payload byte and its physical destructor.
 *   A storage handle grants no pointer, borrow, Source lookup or mutation right.
 */
#ifndef XXIR_BUFFER_STORAGE_H
#define XXIR_BUFFER_STORAGE_H
#include "xxir_value.h"
XR_FUNC XrXirValueStatus xr_xir_buffer_storage_new(XrXirDomain *domain,
    int64_t bytes,int64_t alignment,bool zeroed,XrXirValue *output);
XR_FUNC XrXirValueStatus xr_xir_buffer_storage_length(const XrXirValue *storage,int64_t *output);
/* Synchronous internal observation borrows immutable metadata and raw bytes.
 * The caller keeps this actual value alive; it creates no executable view. */
XR_FUNC bool xr_xir_buffer_storage_observe(const XrXirValue *storage,
    const void **bytes,size_t *length,size_t *alignment);
#endif // XXIR_BUFFER_STORAGE_H
