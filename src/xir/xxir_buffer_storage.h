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
typedef struct XrXirCellAuthority XrXirCellAuthority;
XR_FUNC XrXirValueStatus xr_xir_buffer_storage_new(XrXirDomain *domain,
    int64_t bytes,int64_t alignment,bool zeroed,XrXirValue *output);
XR_FUNC XrXirValueStatus xr_xir_buffer_storage_length(const XrXirValue *storage,int64_t *output);
XR_FUNC XrXirValueStatus xr_xir_buffer_storage_resize(const XrXirValue *storage,int64_t bytes,bool *output);
XR_FUNC XrXirValueStatus xr_xir_buffer_storage_pointer(const XrXirValue *storage,
    const XrXirTypeArena *arena,XrXirType type,XrXirValue *output);
/* The authenticated caller supplies its real scope. Only origin metadata is
 * retained; this readonly Slice neither copies bytes nor prolongs the payload.
 * The caller must register the created view in that scope before publication. */
XR_FUNC XrXirValueStatus xr_xir_buffer_storage_view(const XrXirValue *storage,
    XrXirType slice,const XrXirCellAuthority *authority,
    XrXirValueAdmission *admission,XrXirValue *output);
/* Synchronous internal observation borrows immutable metadata and raw bytes.
 * The caller keeps this actual value alive; it creates no executable view. */
XR_FUNC bool xr_xir_buffer_storage_observe(const XrXirValue *storage,
    const void **bytes,size_t *length,size_t *alignment);
#endif // XXIR_BUFFER_STORAGE_H
