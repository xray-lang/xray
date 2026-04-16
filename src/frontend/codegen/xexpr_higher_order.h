/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xexpr_higher_order.h - Higher-order function inline compilation
 */

#ifndef XEXPR_HIGHER_ORDER_H
#define XEXPR_HIGHER_ORDER_H

#include "xcompiler.h"
#include "../../base/xdefs.h"


/*
 * Try to inline compile higher-order function call
 * 
 * @param ctx compiler context
 * @param compiler compiler
 * @param call call expression node
 * @param result_reg output parameter, if is higher-order function, write result register here
 * @return true if is higher-order function call, false otherwise
 */
XR_FUNC bool try_compile_higher_order_call(XrCompilerContext *ctx, XrCompiler *compiler, 
                                   CallExprNode *call, int *result_reg);

#endif // XEXPR_HIGHER_ORDER_H
