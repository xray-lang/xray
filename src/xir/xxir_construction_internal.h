/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_construction_internal.h - Private owned construction representation
 *
 * KEY CONCEPT:
 *   Receiving-ledger clones bind exact nominal kinds and field denominators.
 */
#ifndef XXIR_CONSTRUCTION_INTERNAL_H
#define XXIR_CONSTRUCTION_INTERNAL_H
#include "xxir_construction.h"

struct XrXirConstruction {
    XrXirCompileContext context;
    XrXirConstructionRow *rows;
    uint32_t *kinds;
    uint32_t count;
};
/* An owner is mandatory, including when no nominal declarations exist.
 * A live foreign input owner may be borrowed. Checks and copies charge the
 * receiver; published copies own all facts on that receiver's ledger. */
XR_FUNC XrXirStatus xir_construction_shape(const XrXirCompileContext *context,
    const XrXirTypes *types, const XrXirConstruction *construction);
XR_FUNC XrXirStatus xir_construction_clone(const XrXirCompileContext *context,
    const XrXirTypes *types, const XrXirConstruction *source,
    XrXirConstruction **output);
XR_FUNC XrXirStatus xir_construction_empty(const XrXirCompileContext *context,
    const XrXirTypes *types, XrXirConstruction **output);
XR_FUNC XrXirStatus xir_construction_verify(const XrXirCompileContext *context,
    const XrXirModule *module, const XrXirConstruction *construction);
#endif // XXIR_CONSTRUCTION_INTERNAL_H
