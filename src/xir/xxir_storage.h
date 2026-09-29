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
} XrXirStorageLayout;

/* Requires a verified pool and exact caller-owned destinations. Construction
 * may change destination bytes on failure; callers publish only on success.
 * Work is consumed on success; bounded temporary scratch is always released. */
XR_FUNC XrXirStatus xr_xir_storage_layouts(const XrXirTypes *types,
    const XrXirTarget *target, XrXirBudget *remaining,
    XrXirStorageLayout *layouts, uint32_t count);
#endif // XXIR_STORAGE_H
