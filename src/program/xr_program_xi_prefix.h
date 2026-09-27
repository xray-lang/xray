/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_xi_prefix.h - Exact membership in an immutable Xi block prefix
 *
 * KEY CONCEPT:
 *   Reuse the previous match for ordered or repeated initializer operands.
 *   A wrapped search still checks the entire prefix for arbitrary source order.
 */

#ifndef XR_PROGRAM_XI_PREFIX_H
#define XR_PROGRAM_XI_PREFIX_H

#include "../ir/xi.h"

static inline bool xr_program_xi_prefix_contains(const XiBlock *block, uint32_t limit,
                                                const XiValue *value, uint32_t *cursor) {
    if (!block || !block->values || !value || !cursor || limit > block->nvalues)
        return false;
    uint32_t start = *cursor < limit ? *cursor : 0u;
    for (uint32_t index = start; index < limit; ++index) {
        if (block->values[index] == value) {
            *cursor = index;
            return true;
        }
    }
    for (uint32_t index = 0u; index < start; ++index) {
        if (block->values[index] == value) {
            *cursor = index;
            return true;
        }
    }
    return false;
}

#endif // XR_PROGRAM_XI_PREFIX_H
