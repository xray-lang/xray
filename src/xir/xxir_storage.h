/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_storage.h - Presealed canonical storage layouts for closed type pools
 *
 * KEY CONCEPT:
 *   Layout metadata owns no execution state and grants no field authority.
 */
#ifndef XXIR_STORAGE_H
#define XXIR_STORAGE_H
#include "xxir.h"

typedef struct XrXirStorageLayout {
    XrXirLayout value;
    const uint32_t *field_offsets;
    uint32_t field_count;
    uint32_t depth, owned_depth, tag_bytes;
    /* Only CLASS has a body; value remains one retained identity handle. */
    XrXirLayout body;
} XrXirStorageLayout;

/* Requires a verified pool and exact caller-owned destinations. Failure preserves destination bytes.
 * Allocations and work share the caller ledger; temporary storage is physically
 * released on every return, while cumulative allocation and work remain charged. */
XR_FUNC XrXirStatus xr_xir_compile_storage_layouts(const XrXirCompileContext *compile_context, const XrXirTypes *types, const XrXirTarget *target, XrXirStorageLayout *layouts, uint32_t count);
#endif // XXIR_STORAGE_H
