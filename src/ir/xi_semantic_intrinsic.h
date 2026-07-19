/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu
 * Licensed under the MIT License
 *
 * xi_semantic_intrinsic.h - Xa semantic intrinsic to Xi contract bridge
 */

#ifndef XI_SEMANTIC_INTRINSIC_H
#define XI_SEMANTIC_INTRINSIC_H

#include "xi.h"
#include "../frontend/analyzer/xa_intrinsic_registry.h"
#include <stddef.h>

XR_FUNC XiOp xi_semantic_intrinsic_op(const XaIntrinsicDesc *desc);
XR_FUNC bool xi_semantic_intrinsic_verify_value(const XiValue *value, XiStage stage, char *error,
                                                size_t error_size);

#endif  // XI_SEMANTIC_INTRINSIC_H
