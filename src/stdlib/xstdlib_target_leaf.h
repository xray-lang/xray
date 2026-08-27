/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstdlib_target_leaf.h - Runtime-neutral typed stdlib leaf provider
 */

#ifndef XSTDLIB_TARGET_LEAF_H
#define XSTDLIB_TARGET_LEAF_H

#include "xstdlib_defs_generated.h"
#include "../shared/xr_os_core.h"
#include <stdbool.h>
#include <stdint.h>

/* The verified TargetPlan selects one generated numeric leaf kind. This
 * provider never resolves a module or member spelling and owns no Xi or
 * SemanticPlan fallback. */
static inline bool xr_stdlib_target_leaf_execute_i64(uint16_t kind, const int64_t *arguments,
                                                     uint16_t argument_count, int64_t *result) {
    if (!result || argument_count != 0 || arguments)
        return false;
    switch ((XrStdlibTargetLeafKind) kind) {
        case XR_STDLIB_TARGET_LEAF_I64_GETPID:
            *result = xr_os_core_getpid();
            return true;
        default:
            return false;
    }
}

#endif /* XSTDLIB_TARGET_LEAF_H */
