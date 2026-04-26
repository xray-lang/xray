/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xconst_fold.h - Compile-time constant folding
 */

#ifndef XCONST_FOLD_H
#define XCONST_FOLD_H

#include "../parser/xast.h"
#include "../../runtime/value/xvalue.h"
#include "xcompiler_context.h"
#include "../../base/xdefs.h"

typedef struct {
    bool success;
    XrValue value;
} XrConstEvalResult;

XR_FUNC XrConstEvalResult xr_const_eval(XrayIsolate *X, AstNode *expr);
XR_FUNC XrConstEvalResult xr_const_eval_with_ctx(XrCompilerContext *ctx, AstNode *expr);
XR_FUNC bool xr_is_const_expr(AstNode *expr);

#endif  // XCONST_FOLD_H
