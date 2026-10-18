/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_task_shapes.h - Shared closed carrier subset for ordinary worker transport
 *
 * KEY CONCEPT:
 *   Shape grants no Sendable or execution proof. GO and sealed Program checks
 *   independently prove those obligations before a child is published.
 */
#ifndef XXIR_TASK_SHAPES_H
#define XXIR_TASK_SHAPES_H
#include "xxir_types.h"
static inline bool xr_xir_task_hash_shape(const XrXirTypes *types, XrXirType type) {
    const XrXirTypeNode *node=xr_xir_type_node(types,type);
    return node && !xr_xir_type_span(types,type) &&
        (node->kind==XR_XIR_TYPE_MAP || node->kind==XR_XIR_TYPE_SET);
}
#endif // XXIR_TASK_SHAPES_H
